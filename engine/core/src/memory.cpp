#include "nexus/core/memory.h"

#include <filesystem>
#include <stdexcept>
#include <string>

// ── Platform selection ──────────────────────────────────────────────────────
// File mapping is available on Windows and the POSIX platforms (Linux, macOS,
// Android).  WebAssembly lacks a meaningful file-backed mapping, so it uses a
// heap fallback that keeps the public behaviour identical.
#if defined(_WIN32)
    #define NEXUS_MMAP_WINDOWS 1
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#elif defined(__EMSCRIPTEN__)
    #define NEXUS_MMAP_HEAP_FALLBACK 1
#else
    #define NEXUS_MMAP_POSIX 1
    #include <fcntl.h>
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace nexus {

namespace {

/// Round `value` up to a multiple of `page` (page is a power of two).
std::size_t round_to_page(std::size_t value, std::size_t page) noexcept {
    if (page == 0) {
        return value;
    }
    return (value + (page - 1)) & ~(page - 1);
}

std::size_t query_page_size() noexcept {
#if defined(NEXUS_MMAP_WINDOWS)
    SYSTEM_INFO info{};
    ::GetSystemInfo(&info);
    return static_cast<std::size_t>(info.dwPageSize);
#elif defined(NEXUS_MMAP_POSIX)
    const long sz = ::sysconf(_SC_PAGESIZE);
    return sz > 0 ? static_cast<std::size_t>(sz) : 4096u;
#else
    return 4096u;
#endif
}

} // namespace

// ── Construction ─────────────────────────────────────────────────────────────

MmapFileAllocator::MmapFileAllocator(std::size_t capacity, std::string path)
    : m_path{std::move(path)} {
    m_page_size = query_page_size();
    m_capacity  = round_to_page(capacity == 0 ? 1 : capacity, m_page_size);

#if defined(NEXUS_MMAP_WINDOWS)
    std::string file_path = m_path;
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    if (file_path.empty()) {
        char temp_dir[MAX_PATH];
        char temp_file[MAX_PATH];
        if (::GetTempPathA(MAX_PATH, temp_dir) == 0 ||
            ::GetTempFileNameA(temp_dir, "nxs", 0, temp_file) == 0) {
            throw std::runtime_error{"MmapFileAllocator: failed to create temp file"};
        }
        file_path   = temp_file;
        m_owns_file = true;
        // FILE_ATTRIBUTE_TEMPORARY lets the OS keep the file out of physical
        // writes where possible; DELETE_ON_CLOSE removes it automatically.
        flags = FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE;
    }

    HANDLE file = ::CreateFileA(file_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                file_path == m_path ? OPEN_ALWAYS : OPEN_EXISTING,
                                flags, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw std::runtime_error{"MmapFileAllocator: CreateFile failed"};
    }
    m_file_handle = file;
    m_path        = file_path;

    const DWORD size_hi = static_cast<DWORD>(m_capacity >> 32);
    const DWORD size_lo = static_cast<DWORD>(m_capacity & 0xFFFFFFFFu);
    HANDLE mapping = ::CreateFileMappingA(file, nullptr, PAGE_READWRITE,
                                          size_hi, size_lo, nullptr);
    if (!mapping) {
        destroy();
        throw std::runtime_error{"MmapFileAllocator: CreateFileMapping failed"};
    }
    m_map_handle = mapping;

    void* base = ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, m_capacity);
    if (!base) {
        destroy();
        throw std::runtime_error{"MmapFileAllocator: MapViewOfFile failed"};
    }
    m_base   = base;
    m_mapped = true;

#elif defined(NEXUS_MMAP_POSIX)
    if (m_path.empty()) {
        std::filesystem::path tmpl =
            std::filesystem::temp_directory_path() / "nexus_mmap_XXXXXX";
        std::string tmpl_str = tmpl.string();
        m_fd = ::mkstemp(tmpl_str.data());
        if (m_fd < 0) {
            throw std::runtime_error{"MmapFileAllocator: mkstemp failed"};
        }
        // Unlink immediately: the descriptor keeps the backing store alive, but
        // it disappears from the filesystem and is reclaimed on close/crash.
        ::unlink(tmpl_str.c_str());
        m_owns_file = false; // already unlinked
        m_path.clear();
    } else {
        m_fd = ::open(m_path.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
        if (m_fd < 0) {
            throw std::runtime_error{"MmapFileAllocator: open failed"};
        }
        m_owns_file = false; // caller-owned, leave it on disk
    }

    if (::ftruncate(m_fd, static_cast<off_t>(m_capacity)) != 0) {
        destroy();
        throw std::runtime_error{"MmapFileAllocator: ftruncate failed"};
    }

    void* base = ::mmap(nullptr, m_capacity, PROT_READ | PROT_WRITE,
                        MAP_SHARED, m_fd, 0);
    if (base == MAP_FAILED) {
        destroy();
        throw std::runtime_error{"MmapFileAllocator: mmap failed"};
    }
    m_base   = base;
    m_mapped = true;

#else // NEXUS_MMAP_HEAP_FALLBACK
    m_base = detail::aligned_malloc(m_capacity, m_page_size);
    if (!m_base) {
        throw std::bad_alloc{};
    }
    m_mapped = false;
#endif
}

// ── Move / destruction ───────────────────────────────────────────────────────

void MmapFileAllocator::move_from(MmapFileAllocator& other) noexcept {
    m_base        = other.m_base;
    m_capacity    = other.m_capacity;
    m_offset      = other.m_offset;
    m_page_size   = other.m_page_size;
    m_fd          = other.m_fd;
    m_file_handle = other.m_file_handle;
    m_map_handle  = other.m_map_handle;
    m_path        = std::move(other.m_path);
    m_mapped      = other.m_mapped;
    m_owns_file   = other.m_owns_file;

    other.m_base        = nullptr;
    other.m_capacity    = 0;
    other.m_offset      = 0;
    other.m_fd          = -1;
    other.m_file_handle = nullptr;
    other.m_map_handle  = nullptr;
    other.m_mapped      = false;
    other.m_owns_file   = false;
}

MmapFileAllocator::MmapFileAllocator(MmapFileAllocator&& other) noexcept {
    move_from(other);
}

MmapFileAllocator& MmapFileAllocator::operator=(MmapFileAllocator&& other) noexcept {
    if (this != &other) {
        destroy();
        move_from(other);
    }
    return *this;
}

MmapFileAllocator::~MmapFileAllocator() {
    destroy();
}

void MmapFileAllocator::destroy() noexcept {
#if defined(NEXUS_MMAP_WINDOWS)
    if (m_base) {
        ::UnmapViewOfFile(m_base);
    }
    if (m_map_handle) {
        ::CloseHandle(static_cast<HANDLE>(m_map_handle));
    }
    if (m_file_handle) {
        ::CloseHandle(static_cast<HANDLE>(m_file_handle));
    }
    // FILE_FLAG_DELETE_ON_CLOSE removes temp files; remove caller-owned-temp
    // paths explicitly when requested.
    if (m_owns_file && !m_path.empty()) {
        ::DeleteFileA(m_path.c_str());
    }
#elif defined(NEXUS_MMAP_POSIX)
    if (m_base && m_base != MAP_FAILED) {
        ::munmap(m_base, m_capacity);
    }
    if (m_fd >= 0) {
        ::close(m_fd);
    }
    if (m_owns_file && !m_path.empty()) {
        ::unlink(m_path.c_str());
    }
#else
    detail::aligned_free(m_base);
#endif
    m_base        = nullptr;
    m_map_handle  = nullptr;
    m_file_handle = nullptr;
    m_fd          = -1;
    m_offset      = 0;
    m_capacity    = 0;
    m_mapped      = false;
    m_owns_file   = false;
}

// ── Allocation ───────────────────────────────────────────────────────────────

void* MmapFileAllocator::allocate(std::size_t size, std::size_t alignment) noexcept {
    NEXUS_ASSERT(is_power_of_two(alignment), "alignment must be a power of two");
    if (!m_base) {
        return nullptr;
    }
    const auto base       = reinterpret_cast<std::uintptr_t>(m_base);
    const auto aligned    = align_up_addr(base + m_offset, alignment);
    const std::size_t end = static_cast<std::size_t>(aligned - base) + size;
    if (end > m_capacity) {
        return nullptr;
    }
    m_offset = end;
    return reinterpret_cast<void*>(aligned);
}

void MmapFileAllocator::release_physical_memory() noexcept {
    if (!m_base || !m_mapped) {
        return;
    }
    const std::size_t tail_start = round_to_page(m_offset, m_page_size);
    if (tail_start >= m_capacity) {
        return;
    }
    const std::size_t tail_len = m_capacity - tail_start;
    auto* tail = static_cast<u8*>(m_base) + tail_start;

#if defined(NEXUS_MMAP_WINDOWS)
    // MEM_RESET tells the OS the tail's contents need not be preserved, so it
    // can reclaim the physical pages without writing them back.
    ::VirtualAlloc(tail, tail_len, MEM_RESET, PAGE_READWRITE);
#elif defined(NEXUS_MMAP_POSIX)
    // MADV_DONTNEED drops the resident pages; they fault back in (zero-filled
    // from the truncated file) on next access.
    ::madvise(tail, tail_len, MADV_DONTNEED);
#else
    (void)tail;
    (void)tail_len;
#endif
}

} // namespace nexus
