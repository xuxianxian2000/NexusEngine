#pragma once

#include "nexus/core/types.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
    #include <malloc.h> // _aligned_malloc / _aligned_free
#endif

namespace nexus {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// True if `value` is a power of two (and non-zero).
inline constexpr bool is_power_of_two(std::size_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

/// Align `value` up to the nearest multiple of `alignment`.
/// `alignment` must be a power of two.
inline constexpr std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
    return (value + (alignment - 1)) & ~(alignment - 1);
}

/// Align the pointer-sized integer `addr` up to `alignment` (power of two).
inline std::uintptr_t align_up_addr(std::uintptr_t addr, std::size_t alignment) noexcept {
    const auto a = static_cast<std::uintptr_t>(alignment);
    return (addr + (a - 1)) & ~(a - 1);
}

namespace detail {

/// Portable aligned allocation.  `alignment` must be a power of two; it is
/// bumped to at least `sizeof(void*)` so it is valid for every backend.
/// Returns nullptr on failure.  Must be released with `aligned_free`.
inline void* aligned_malloc(std::size_t size, std::size_t alignment) noexcept {
    if (alignment < sizeof(void*)) {
        alignment = sizeof(void*);
    }
#if defined(_WIN32)
    return _aligned_malloc(size, alignment);
#else
    void* ptr = nullptr;
    if (::posix_memalign(&ptr, alignment, size) != 0) {
        return nullptr;
    }
    return ptr;
#endif
}

/// Release memory obtained from `aligned_malloc`.
inline void aligned_free(void* ptr) noexcept {
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

} // namespace detail

// ---------------------------------------------------------------------------
// IAllocator
// ---------------------------------------------------------------------------
/// Common interface implemented by every engine allocator so that subsystems
/// can accept an allocator polymorphically (e.g. swap a heap-backed arena for
/// the file-backed `MmapFileAllocator` without changing call sites).
///
/// `allocate` returns nullptr (or throws, depending on the concrete allocator)
/// when it cannot satisfy a request.  `deallocate` is a no-op for the bump /
/// linear style allocators; only pool-style allocators reclaim individual
/// objects.  `reset` releases everything the allocator is holding at once.
class IAllocator {
public:
    IAllocator()          = default;
    virtual ~IAllocator() = default;

    NEXUS_NON_COPYABLE(IAllocator)

    /// Allocate `size` bytes aligned to `alignment` (must be a power of two).
    [[nodiscard]] virtual void* allocate(std::size_t size, std::size_t alignment) = 0;

    /// Return a block previously obtained from `allocate`.
    virtual void deallocate(void* ptr) noexcept = 0;

    /// Reclaim every outstanding allocation in one shot.
    virtual void reset() noexcept {}
};

// ---------------------------------------------------------------------------
// LinearAllocator
// ---------------------------------------------------------------------------
/// Bump / linear allocator.  Allocates sequentially from a pre-allocated
/// contiguous buffer.  Individual de-allocations are not supported; call
/// reset() to free everything at once.
class LinearAllocator final : public IAllocator {
public:
    NEXUS_NON_COPYABLE(LinearAllocator)

    static constexpr std::size_t DEFAULT_ALIGNMENT = alignof(std::max_align_t);

    explicit LinearAllocator(std::size_t capacity)
        : m_capacity{capacity} {
        // Over-align the backing buffer to a cache line so callers asking for
        // up-to-64-byte alignment never lose capacity to base mis-alignment.
        m_buffer = static_cast<u8*>(detail::aligned_malloc(capacity, 64));
        if (!m_buffer) {
            throw std::bad_alloc{};
        }
    }

    LinearAllocator(LinearAllocator&& other) noexcept
        : m_buffer{other.m_buffer}
        , m_capacity{other.m_capacity}
        , m_offset{other.m_offset} {
        other.m_buffer   = nullptr;
        other.m_capacity = 0;
        other.m_offset   = 0;
    }

    LinearAllocator& operator=(LinearAllocator&& other) noexcept {
        if (this != &other) {
            detail::aligned_free(m_buffer);
            m_buffer         = other.m_buffer;
            m_capacity       = other.m_capacity;
            m_offset         = other.m_offset;
            other.m_buffer   = nullptr;
            other.m_capacity = 0;
            other.m_offset   = 0;
        }
        return *this;
    }

    ~LinearAllocator() override {
        detail::aligned_free(m_buffer);
    }

    /// Allocate `size` bytes with the given `alignment` (must be power of 2).
    /// Returns nullptr if the allocator is exhausted.
    [[nodiscard]] void* allocate(std::size_t size,
                                 std::size_t alignment = DEFAULT_ALIGNMENT) noexcept override {
        NEXUS_ASSERT(is_power_of_two(alignment), "alignment must be a power of two");

        // Align the absolute address, not just the offset: this stays correct
        // even when `alignment` exceeds the backing buffer's base alignment.
        const auto base       = reinterpret_cast<std::uintptr_t>(m_buffer);
        const auto aligned    = align_up_addr(base + m_offset, alignment);
        const std::size_t end = static_cast<std::size_t>(aligned - base) + size;
        if (end > m_capacity) {
            return nullptr;
        }
        m_offset = end;
        return reinterpret_cast<void*>(aligned);
    }

    /// Bump allocators do not reclaim individual blocks.
    void deallocate(void* /*ptr*/) noexcept override {}

    /// Reset the allocator – all previous allocations become invalid.
    void reset() noexcept override { m_offset = 0; }

    /// Number of bytes currently in use (including alignment padding).
    [[nodiscard]] std::size_t used()     const noexcept { return m_offset; }

    /// Total capacity of the backing buffer in bytes.
    [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }

private:
    friend class ScopedArena;

    u8*         m_buffer   = nullptr;
    std::size_t m_capacity = 0;
    std::size_t m_offset   = 0;
};

// ---------------------------------------------------------------------------
// ArenaAllocator
// ---------------------------------------------------------------------------
/// Arena allocator that grows by appending new blocks when the current one is
/// exhausted.  Designed for per-frame allocations where everything is freed at
/// the end of the frame via reset().
class ArenaAllocator final : public IAllocator {
public:
    NEXUS_NON_COPYABLE(ArenaAllocator)

    static constexpr std::size_t DEFAULT_BLOCK_SIZE = 1024 * 1024; // 1 MB
    static constexpr std::size_t DEFAULT_ALIGNMENT  = alignof(std::max_align_t);

    explicit ArenaAllocator(std::size_t block_size = DEFAULT_BLOCK_SIZE)
        : m_block_size{block_size} {
        allocate_block(m_block_size);
    }

    ArenaAllocator(ArenaAllocator&& other) noexcept
        : m_blocks{std::move(other.m_blocks)}
        , m_block_size{other.m_block_size}
        , m_current_offset{other.m_current_offset} {
        other.m_current_offset = 0;
    }

    ArenaAllocator& operator=(ArenaAllocator&& other) noexcept {
        if (this != &other) {
            free_all();
            m_blocks               = std::move(other.m_blocks);
            m_block_size           = other.m_block_size;
            m_current_offset       = other.m_current_offset;
            other.m_current_offset = 0;
        }
        return *this;
    }

    ~ArenaAllocator() override {
        free_all();
    }

    /// Allocate `size` bytes with the given `alignment`.
    /// Grows by allocating a new block if the current block is exhausted.
    [[nodiscard]] void* allocate(std::size_t size,
                                 std::size_t alignment = DEFAULT_ALIGNMENT) override {
        NEXUS_ASSERT(is_power_of_two(alignment), "alignment must be a power of two");
        NEXUS_ASSERT(!m_blocks.empty(), "ArenaAllocator has no blocks");

        if (void* ptr = try_allocate(m_blocks.back(), size, alignment)) {
            return ptr;
        }

        // Current block can't satisfy the request – allocate a new one large
        // enough to hold the request plus worst-case alignment padding.
        std::size_t new_block_size = m_block_size;
        if (size + alignment > new_block_size) {
            new_block_size = size + alignment;
        }
        allocate_block(new_block_size);

        void* ptr = try_allocate(m_blocks.back(), size, alignment);
        NEXUS_ASSERT(ptr != nullptr, "fresh arena block failed to satisfy request");
        return ptr;
    }

    /// Arena reclaims everything via reset(), not per-block.
    void deallocate(void* /*ptr*/) noexcept override {}

    /// Free all blocks except the first one and reset offsets.
    void reset() noexcept override {
        if (m_blocks.empty()) return;

        // Keep the first block, free the rest.
        for (std::size_t i = 1; i < m_blocks.size(); ++i) {
            detail::aligned_free(m_blocks[i].memory);
        }
        m_blocks.resize(1);
        m_current_offset = 0;
    }

private:
    struct Block {
        u8*         memory = nullptr;
        std::size_t size   = 0;
    };

    /// Try to carve `size` bytes (aligned) out of `block`, advancing the
    /// current offset.  Returns nullptr if the block lacks room.
    void* try_allocate(Block& block, std::size_t size, std::size_t alignment) noexcept {
        const auto base       = reinterpret_cast<std::uintptr_t>(block.memory);
        const auto aligned    = align_up_addr(base + m_current_offset, alignment);
        const std::size_t end = static_cast<std::size_t>(aligned - base) + size;
        if (end > block.size) {
            return nullptr;
        }
        m_current_offset = end;
        return reinterpret_cast<void*>(aligned);
    }

    void allocate_block(std::size_t size) {
        Block block;
        // Cache-line align blocks so absolute-address alignment never needs to
        // skip past the start for the common (<=64 byte) alignment requests.
        block.memory = static_cast<u8*>(detail::aligned_malloc(size, 64));
        if (!block.memory) {
            throw std::bad_alloc{};
        }
        block.size = size;
        m_blocks.push_back(block);
        m_current_offset = 0;
    }

    void free_all() noexcept {
        for (auto& b : m_blocks) {
            detail::aligned_free(b.memory);
        }
        m_blocks.clear();
        m_current_offset = 0;
    }

    std::vector<Block> m_blocks;
    std::size_t        m_block_size      = DEFAULT_BLOCK_SIZE;
    std::size_t        m_current_offset  = 0;
};

// ---------------------------------------------------------------------------
// PoolAllocator
// ---------------------------------------------------------------------------
/// Fixed-size object pool backed by a free list.
/// `ObjectSize` is the size of each slot in bytes.
/// `Alignment` is the alignment requirement for each slot.
template <std::size_t ObjectSize, std::size_t Alignment = alignof(std::max_align_t)>
class PoolAllocator final : public IAllocator {
public:
    NEXUS_NON_COPYABLE(PoolAllocator)

    static_assert(is_power_of_two(Alignment), "PoolAllocator Alignment must be a power of two");

    static constexpr std::size_t SLOT_SIZE =
        align_up(ObjectSize < sizeof(void*) ? sizeof(void*) : ObjectSize, Alignment);

    /// Create a pool that can hold `count` objects.
    explicit PoolAllocator(std::size_t count)
        : m_count{count} {
        // `aligned_malloc` requires no size/alignment relationship, but SLOT_SIZE
        // is already a multiple of Alignment so every slot stays aligned.
        const std::size_t total = SLOT_SIZE * count;
        m_buffer = static_cast<u8*>(detail::aligned_malloc(total, Alignment));
        if (!m_buffer) {
            throw std::bad_alloc{};
        }

        // Build the free list.
        m_free_head = nullptr;
        for (std::size_t i = count; i > 0; --i) {
            auto* node  = reinterpret_cast<FreeNode*>(m_buffer + (i - 1) * SLOT_SIZE);
            node->next  = m_free_head;
            m_free_head = node;
        }
    }

    PoolAllocator(PoolAllocator&& other) noexcept
        : m_buffer{other.m_buffer}
        , m_free_head{other.m_free_head}
        , m_count{other.m_count} {
        other.m_buffer    = nullptr;
        other.m_free_head = nullptr;
        other.m_count     = 0;
    }

    PoolAllocator& operator=(PoolAllocator&& other) noexcept {
        if (this != &other) {
            detail::aligned_free(m_buffer);
            m_buffer          = other.m_buffer;
            m_free_head       = other.m_free_head;
            m_count           = other.m_count;
            other.m_buffer    = nullptr;
            other.m_free_head = nullptr;
            other.m_count     = 0;
        }
        return *this;
    }

    ~PoolAllocator() override {
        detail::aligned_free(m_buffer);
    }

    /// Allocate a single slot.  Returns nullptr if the pool is exhausted.
    [[nodiscard]] void* allocate() noexcept {
        if (!m_free_head) {
            return nullptr;
        }
        FreeNode* node = m_free_head;
        m_free_head    = node->next;
        return static_cast<void*>(node);
    }

    /// IAllocator entry point: `size`/`alignment` must fit a single slot.
    [[nodiscard]] void* allocate(std::size_t size, std::size_t alignment) noexcept override {
        NEXUS_ASSERT(size <= ObjectSize, "PoolAllocator: requested size exceeds slot size");
        NEXUS_ASSERT(alignment <= Alignment, "PoolAllocator: requested alignment exceeds slot alignment");
        (void)size;
        (void)alignment;
        return allocate();
    }

    /// Return a previously allocated slot to the pool.
    void deallocate(void* ptr) noexcept override {
        if (!ptr) return;
        NEXUS_ASSERT(
            ptr >= m_buffer && ptr < m_buffer + SLOT_SIZE * m_count,
            "PoolAllocator::deallocate – pointer not from this pool");
        auto* node  = static_cast<FreeNode*>(ptr);
        node->next  = m_free_head;
        m_free_head = node;
    }

    /// Total number of slots in the pool.
    [[nodiscard]] std::size_t count() const noexcept { return m_count; }

private:
    struct FreeNode {
        FreeNode* next = nullptr;
    };

    u8*         m_buffer    = nullptr;
    FreeNode*   m_free_head = nullptr;
    std::size_t m_count     = 0;
};

// ---------------------------------------------------------------------------
// ScopedArena
// ---------------------------------------------------------------------------
/// RAII helper that saves the current offset of a LinearAllocator on
/// construction and restores it on destruction, effectively freeing any
/// allocations made within the scope.
class ScopedArena {
public:
    NEXUS_NON_COPYABLE(ScopedArena)
    NEXUS_NON_MOVABLE(ScopedArena)

    explicit ScopedArena(LinearAllocator& allocator) noexcept
        : m_allocator{allocator}
        , m_saved_offset{allocator.m_offset} {}

    ~ScopedArena() noexcept {
        m_allocator.m_offset = m_saved_offset;
    }

    /// Convenience: forward allocations to the underlying LinearAllocator.
    void* allocate(std::size_t size, std::size_t alignment = LinearAllocator::DEFAULT_ALIGNMENT) noexcept {
        return m_allocator.allocate(size, alignment);
    }

private:
    LinearAllocator& m_allocator;
    std::size_t      m_saved_offset;
};

// ---------------------------------------------------------------------------
// MmapFileAllocator
// ---------------------------------------------------------------------------
/// File-backed (memory-mapped) linear allocator.
///
/// The backing store is a memory-mapped file rather than the process heap, so
/// the operating system is free to evict cold regions to disk instead of
/// keeping them resident in physical RAM.  This trades a little latency for a
/// much smaller resident-set size, which is ideal for large, mostly-cold
/// working sets: asset-import staging buffers, level-load scratch, offline
/// bakers and the like.
///
/// Allocation is bump / linear style — individual de-allocations are no-ops.
/// Call reset() to rewind the cursor (cheap), or release_physical_memory() to
/// hand the unused tail's resident pages back to the OS while keeping the
/// virtual reservation (and therefore previously returned pointers) valid.
///
/// On platforms without file mapping (e.g. WebAssembly) it transparently falls
/// back to a heap-backed buffer, so callers do not need to special-case it.
class MmapFileAllocator final : public IAllocator {
public:
    NEXUS_NON_COPYABLE(MmapFileAllocator)

    static constexpr std::size_t DEFAULT_ALIGNMENT = alignof(std::max_align_t);

    /// Create an allocator backed by a memory-mapped file of (at least)
    /// `capacity` bytes.  When `path` is empty a unique temporary file is
    /// created and removed automatically.  Throws std::runtime_error /
    /// std::bad_alloc on failure.
    explicit MmapFileAllocator(std::size_t capacity, std::string path = {});

    MmapFileAllocator(MmapFileAllocator&& other) noexcept;
    MmapFileAllocator& operator=(MmapFileAllocator&& other) noexcept;
    ~MmapFileAllocator() override;

    [[nodiscard]] void* allocate(std::size_t size,
                                 std::size_t alignment = DEFAULT_ALIGNMENT) noexcept override;
    void deallocate(void* /*ptr*/) noexcept override {}

    /// Rewind the allocation cursor to the start.  Cheap; does not return
    /// physical memory to the OS (use release_physical_memory() for that).
    void reset() noexcept override { m_offset = 0; }

    /// Advise the OS that the currently-unused tail of the mapping is no longer
    /// needed and drop its resident pages.  The virtual mapping is preserved;
    /// pages fault back in (zero-filled) on next access.  No-op on the heap
    /// fallback path.
    void release_physical_memory() noexcept;

    [[nodiscard]] std::size_t used()       const noexcept { return m_offset; }
    [[nodiscard]] std::size_t capacity()   const noexcept { return m_capacity; }
    [[nodiscard]] bool        is_mapped()  const noexcept { return m_mapped; }
    [[nodiscard]] const std::string& path() const noexcept { return m_path; }

private:
    void destroy() noexcept;
    void move_from(MmapFileAllocator& other) noexcept;

    void*       m_base        = nullptr; // mapped (or heap) base address
    std::size_t m_capacity    = 0;       // usable bytes (rounded up to a page)
    std::size_t m_offset      = 0;       // bump cursor
    std::size_t m_page_size   = 0;       // OS page size used for advice rounding
    int         m_fd          = -1;      // POSIX file descriptor (-1 if none)
    void*       m_file_handle = nullptr; // Windows file HANDLE
    void*       m_map_handle  = nullptr; // Windows mapping HANDLE
    std::string m_path;                  // backing file path (empty => heap/temp)
    bool        m_mapped      = false;   // true if file-mapped, false if heap fallback
    bool        m_owns_file   = false;   // remove backing file on destroy
};

} // namespace nexus
