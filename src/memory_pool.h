#pragma once
// =============================================================================
// memory_pool.h  —  Stage 4: Lock-Free Fixed-Size Memory Pool
// =============================================================================
//
// CONCEPT: Custom Memory Allocation
//
// The default allocator (new / malloc) uses a general-purpose heap that:
//   • Acquires a mutex on every allocation (thread contention)
//   • Walks fragmented free lists (cache unfriendly)
//   • Can call the OS (extremely slow on the hot path)
//
// In HFT we pre-allocate a pool of fixed-size blocks at startup. At runtime:
//   • Allocation = pop from a lock-free free list (a few nanoseconds)
//   • Deallocation = push back onto the free list (a few nanoseconds)
//   • Zero heap fragmentation (all blocks are the same size)
//   • Memory stays warm in cache between alloc/free cycles
//
// CONCEPT: ABA Problem & Tagged Pointers
//
// A naive lock-free stack has the ABA problem: thread A reads head = X,
// gets suspended; thread B pops X and Y, pushes X back; thread A's CAS
// succeeds even though the list has changed under it.
//
// Solution: tag each pointer with a version counter. Pack (index, version)
// into a single 64-bit value so the CAS is still a single atomic instruction.
// The version counter makes the ABA case distinguishable (version differs).
//
// CONCEPT: Placement new
//
// Instead of `new T(...)` (which allocates + constructs), we:
//   1. Allocate raw memory from the pool  →  T* ptr = pool.allocate()
//   2. Construct in-place               →  new(ptr) T(args...)
//   3. Destroy                          →  ptr->~T()
//   4. Return memory to pool            →  pool.deallocate(ptr)
//
// This separates allocation lifetime from object lifetime, giving full control
// over both — essential in latency-critical systems.
// =============================================================================

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>
#include <cassert>

// ---------------------------------------------------------------------------
// MemoryPool<T, PoolSize>
//
// A fixed-capacity, lock-free, thread-safe pool allocator.
//
// Template parameters:
//   T        — type of objects to allocate
//   PoolSize — maximum number of live objects at any time
//
// Thread safety: allocate() and deallocate() are safe to call from multiple
// threads concurrently (Treiber stack with tagged indices).
// ---------------------------------------------------------------------------
template<typename T, std::size_t PoolSize>
class MemoryPool {
    static_assert(PoolSize > 0 && PoolSize < (1u << 31),
        "PoolSize must be positive and fit in 31 bits");

    // -----------------------------------------------------------------------
    // Block: raw storage for one T, plus a link field for the free list.
    //
    // When the block is FREE:  the first sizeof(uint32_t) bytes of `storage`
    //                          hold the index of the next free block.
    // When the block is LIVE:  `storage` contains a properly constructed T.
    //
    // We use unsigned char[] instead of std::byte[] for C++14 compatibility
    // while still providing legally aliasable raw storage.
    // alignas(alignof(T)) ensures correct alignment for placement new.
    // -----------------------------------------------------------------------
    struct Block {
        alignas(alignof(T)) unsigned char storage[sizeof(T)];
    };

    // -----------------------------------------------------------------------
    // Tagged index: (index, version) packed into one 64-bit word.
    //
    // index   — 32 low bits  — index into blocks_[], or EMPTY if list is empty
    // version — 32 high bits — incremented on every push to defeat ABA
    //
    // Packing into uint64_t allows a single 64-bit CAS which is atomic on
    // all x86-64 / ARM64 platforms (no lock prefix needed if naturally aligned).
    // -----------------------------------------------------------------------
    static constexpr uint32_t EMPTY = UINT32_MAX;

    static constexpr uint64_t pack(uint32_t idx, uint32_t ver) noexcept {
        return (static_cast<uint64_t>(ver) << 32) | idx;
    }
    static constexpr uint32_t idx_of(uint64_t tagged) noexcept {
        return static_cast<uint32_t>(tagged);
    }
    static constexpr uint32_t ver_of(uint64_t tagged) noexcept {
        return static_cast<uint32_t>(tagged >> 32);
    }

    // Read the embedded "next" index from a free block's storage.
    // This is legal: we only access this when the block is not hosting a T,
    // so there is no aliasing with a live object.
    static uint32_t& next_ref(Block& b) noexcept {
        uint32_t result;
        std::memcpy(&result, b.storage, sizeof(uint32_t));
        return *reinterpret_cast<uint32_t*>(b.storage);
    }

    // -----------------------------------------------------------------------
    // Storage
    // -----------------------------------------------------------------------

    // Pool of raw blocks — allocated inline (no extra heap allocation).
    // Aligned to 64 bytes so the first block starts on a cache line.
    alignas(64) Block blocks_[PoolSize];

    // Head of the free list: packed (index, version).
    // alignas(8) ensures the 64-bit CAS is naturally aligned (mandatory on ARM).
    alignas(8) std::atomic<uint64_t> free_head_;

    // Approximate count of live objects (relaxed — metrics only)
    std::atomic<std::size_t> in_use_{0};

    // -----------------------------------------------------------------------
    // Initialize the free list as a singly-linked chain:
    //   blocks_[0] → blocks_[1] → ... → blocks_[PoolSize-1] → EMPTY
    // -----------------------------------------------------------------------
    void init_free_list() noexcept {
        for (uint32_t i = 0; i < static_cast<uint32_t>(PoolSize) - 1; ++i) {
            next_ref(blocks_[i]) = i + 1;
        }
        next_ref(blocks_[PoolSize - 1]) = EMPTY;
        free_head_.store(pack(0, 0), std::memory_order_relaxed);
    }

public:
    MemoryPool() noexcept { init_free_list(); }

    // Not copyable: we hold raw memory and embedded atomics
    MemoryPool(const MemoryPool&)            = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    // -----------------------------------------------------------------------
    // allocate() — pop one block from the free list
    //
    // Returns a pointer to sizeof(T) bytes of correctly aligned raw storage,
    // or nullptr if the pool is exhausted.
    //
    // The caller must construct a T into the returned memory via placement new
    // before using it, and must destroy it before calling deallocate().
    // -----------------------------------------------------------------------
    [[nodiscard]] T* allocate() noexcept {
        uint64_t head = free_head_.load(std::memory_order_acquire);

        while (true) {
            const uint32_t idx = idx_of(head);
            if (idx == EMPTY) {
                return nullptr; // pool exhausted
            }

            // Peek at the next pointer BEFORE the CAS.
            // If the CAS fails, `head` is refreshed automatically.
            const uint32_t next_idx = next_ref(blocks_[idx]);
            const uint32_t new_ver  = ver_of(head) + 1; // bump version → defeat ABA

            if (free_head_.compare_exchange_weak(
                    head, pack(next_idx, new_ver),
                    std::memory_order_release,   // success: publish the pop
                    std::memory_order_acquire))  // failure: see latest head
            {
                in_use_.fetch_add(1, std::memory_order_relaxed);
                return reinterpret_cast<T*>(blocks_[idx].storage);
            }
            // Retry with refreshed head
        }
    }

    // -----------------------------------------------------------------------
    // deallocate() — push a block back onto the free list
    //
    // PRECONDITION: ptr was returned by allocate() and the T has already been
    // destroyed (ptr->~T() called). Use destroy() which handles both.
    // -----------------------------------------------------------------------
    void deallocate(T* ptr) noexcept {
        assert(ptr && "deallocate called with nullptr");

        auto* block = reinterpret_cast<Block*>(ptr);
        const auto idx = static_cast<uint32_t>(block - blocks_);
        assert(idx < PoolSize && "pointer does not belong to this pool");

        uint64_t head = free_head_.load(std::memory_order_acquire);

        while (true) {
            // Link this block to point at the current head
            next_ref(blocks_[idx]) = idx_of(head);
            const uint32_t new_ver = ver_of(head) + 1;

            if (free_head_.compare_exchange_weak(
                    head, pack(idx, new_ver),
                    std::memory_order_release,
                    std::memory_order_acquire))
            {
                in_use_.fetch_sub(1, std::memory_order_relaxed);
                return;
            }
        }
    }

    // -----------------------------------------------------------------------
    // construct() — allocate + placement-new in one step
    //
    // USAGE:
    //   Order* o = pool.construct();          // default-constructed
    //   Order* o = pool.construct(args...);   // forwarded to T's constructor
    // -----------------------------------------------------------------------
    template<typename... Args>
    [[nodiscard]] T* construct(Args&&... args) noexcept(
        noexcept(T(std::forward<Args>(args)...)))
    {
        T* ptr = allocate();
        if (ptr) {
            // Placement new: construct T at address ptr.
            // `new (ptr)` calls the constructor WITHOUT any allocation.
            // We are responsible for calling ptr->~T() before deallocation.
            ::new(static_cast<void*>(ptr)) T(std::forward<Args>(args)...);
        }
        return ptr;
    }

    // -----------------------------------------------------------------------
    // destroy() — explicit destructor + deallocate in one step
    // -----------------------------------------------------------------------
    void destroy(T* ptr) noexcept {
        if (ptr) {
            ptr->~T();          // explicit destructor (paired with placement new)
            deallocate(ptr);
        }
    }

    // -----------------------------------------------------------------------
    // Diagnostics
    // -----------------------------------------------------------------------
    [[nodiscard]] std::size_t capacity()   const noexcept { return PoolSize; }
    [[nodiscard]] std::size_t in_use()     const noexcept {
        return in_use_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t available()  const noexcept {
        const std::size_t used = in_use_.load(std::memory_order_relaxed);
        return used <= PoolSize ? PoolSize - used : 0;
    }
};
