#include <gtest/gtest.h>
#include <nexus/core/memory.h>

#include <cstdint>
#include <cstring>
#include <utility>

namespace nexus::tests {

namespace {
bool is_aligned(const void* p, std::size_t alignment) {
    return (reinterpret_cast<std::uintptr_t>(p) & (alignment - 1)) == 0;
}
} // namespace

TEST(LinearAllocator, AllocateAndReset) {
    LinearAllocator alloc(1024);
    auto* p1 = alloc.allocate(64);
    auto* p2 = alloc.allocate(128);

    EXPECT_NE(p1, nullptr);
    EXPECT_NE(p2, nullptr);
    EXPECT_NE(p1, p2);

    alloc.reset();
    auto* p3 = alloc.allocate(64);
    // After reset, should reuse space from the start
    EXPECT_EQ(p3, p1);
}

TEST(LinearAllocator, OversizeReturnsNull) {
    LinearAllocator alloc(64);
    auto* p = alloc.allocate(128);
    EXPECT_EQ(p, nullptr);
}

TEST(LinearAllocator, HonorsOverAlignment) {
    LinearAllocator alloc(4096);
    for (std::size_t alignment : {16u, 32u, 64u, 128u, 256u}) {
        void* p = alloc.allocate(8, alignment);
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(is_aligned(p, alignment)) << "alignment=" << alignment;
    }
}

TEST(PoolAllocator, AllocateAndDeallocate) {
    PoolAllocator<sizeof(int)> pool(4);
    auto* p1 = pool.allocate();
    auto* p2 = pool.allocate();

    EXPECT_NE(p1, nullptr);
    EXPECT_NE(p2, nullptr);
    EXPECT_NE(p1, p2);

    pool.deallocate(p1);
    auto* p3 = pool.allocate();
    // Should reuse freed slot
    EXPECT_EQ(p3, p1);
}

TEST(ArenaAllocator, GrowsAutomatically) {
    ArenaAllocator arena(64); // Small initial block
    void* ptrs[10];
    for (int i = 0; i < 10; ++i) {
        ptrs[i] = arena.allocate(32); // Will need multiple blocks
        EXPECT_NE(ptrs[i], nullptr);
    }
}

TEST(ArenaAllocator, HonorsOverAlignment) {
    ArenaAllocator arena(256);
    for (int i = 0; i < 8; ++i) {
        void* p = arena.allocate(24, 64);
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(is_aligned(p, 64));
    }
}

// Allocators are usable through the common interface.
TEST(IAllocator, PolymorphicUse) {
    LinearAllocator linear(256);
    IAllocator& alloc = linear;
    void* p = alloc.allocate(32, 16);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(is_aligned(p, 16));
    alloc.reset();
    EXPECT_EQ(alloc.allocate(32, 16), p);
}

TEST(MmapFileAllocator, AllocateResetAndRelease) {
    MmapFileAllocator alloc(1 << 20); // 1 MB, temp-file backed
    EXPECT_GE(alloc.capacity(), static_cast<std::size_t>(1 << 20));

    void* p1 = alloc.allocate(4096, 64);
    void* p2 = alloc.allocate(4096, 64);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    EXPECT_NE(p1, p2);
    EXPECT_TRUE(is_aligned(p1, 64));
    EXPECT_GT(alloc.used(), 0u);

    // Write through the mapping to make sure it is real, writable memory.
    std::memset(p1, 0xAB, 4096);
    EXPECT_EQ(static_cast<unsigned char*>(p1)[0], 0xAB);

    // Returning physical pages must not invalidate the virtual reservation.
    alloc.release_physical_memory();

    alloc.reset();
    EXPECT_EQ(alloc.used(), 0u);
    void* p3 = alloc.allocate(4096, 64);
    EXPECT_EQ(p3, p1);
}

TEST(MmapFileAllocator, OversizeReturnsNull) {
    MmapFileAllocator alloc(4096);
    EXPECT_EQ(alloc.allocate(alloc.capacity() + 1), nullptr);
}

TEST(MmapFileAllocator, MoveTransfersOwnership) {
    MmapFileAllocator a(4096);
    void* p = a.allocate(64);
    ASSERT_NE(p, nullptr);

    MmapFileAllocator b(std::move(a));
    EXPECT_GE(b.capacity(), 4096u);
    EXPECT_EQ(a.capacity(), 0u);   // moved-from is emptied
    EXPECT_EQ(a.used(), 0u);
    void* p2 = b.allocate(64);     // b owns the mapping now
    EXPECT_NE(p2, nullptr);
}

} // namespace nexus::tests
