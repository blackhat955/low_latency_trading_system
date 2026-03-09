#pragma once
// =============================================================================
// ring_buffer.h  —  Stage 2 & 3: Lock-Free SPSC Ring Buffer
// =============================================================================
//
// CONCEPT: Lock-Free Programming with std::atomic
//
// A mutex-based queue forces two expensive operations on every push/pop:
//   1. A system call (or at least a CAS loop) to acquire the lock
//   2. A full memory fence that flushes CPU store buffers
//
// For a Single-Producer / Single-Consumer (SPSC) ring buffer we can do much
// better because only ONE thread ever writes `head_` and only ONE thread ever
// writes `tail_`. This means we never have write-write contention, and we only
// need acquire/release semantics — not sequential consistency.
//
// Memory ordering recap:
//   memory_order_relaxed  — no ordering guarantee; cheapest
//   memory_order_acquire  — all subsequent reads see stores from the
//                           releasing thread (pairs with release)
//   memory_order_release  — all preceding writes are visible to the
//                           acquiring thread (pairs with acquire)
//   memory_order_seq_cst  — total order; expensive; default for atomic<>
//
// The SPSC push/pop protocol:
//   push (producer only):
//     1. Read own head with relaxed (no one else writes head)
//     2. Check tail_cache; if needed, refresh from tail_ with ACQUIRE
//        so that slot writes by a previous producer are visible.
//     3. Write the data slot (plain store — no atomic needed for data).
//     4. Publish the new head with RELEASE so the consumer sees the data.
//
//   pop (consumer only):
//     1. Read own tail with relaxed.
//     2. Check head_cache; if needed, refresh from head_ with ACQUIRE
//        so we see the data written before the release store.
//     3. Copy the data from the slot.
//     4. Advance tail with RELEASE so the producer can reuse the slot.
//
// CONCEPT: False Sharing Avoidance
//
// If `head_` and `tail_` share a cache line the producer's write to head_
// invalidates the consumer's cache line containing tail_, forcing it to
// reload — even though tail_ itself didn't change. This is "false sharing"
// and can halve throughput.
//
// Solution: put head_ and tail_ on *separate* cache lines using alignas(64)
// and explicit padding. We also cache the remote pointer locally to further
// reduce cross-thread reads.
// =============================================================================

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

// ---------------------------------------------------------------------------
// LockFreeRingBuffer<T, Capacity>
//
// Requirements:
//   • T must be trivially copyable (no lock guards inside the copy)
//   • Capacity must be a power of 2 (allows fast modulo via bitmask)
//   • Exactly one producer thread and one consumer thread
//
// Template parameters:
//   T        — element type (Order in this project)
//   Capacity — number of slots; must be power of 2
// ---------------------------------------------------------------------------
template<typename T, std::size_t Capacity>
class LockFreeRingBuffer {

    static_assert(std::is_trivially_copyable<T>::value,
        "T must be trivially copyable for safe lock-free copy");

    static_assert((Capacity & (Capacity - 1)) == 0,
        "Capacity must be a power of 2 for bitmask modulo");

    static_assert(Capacity >= 2,
        "Capacity must be at least 2");

    // Mask for fast modulo: (index + 1) & MASK == (index + 1) % Capacity
    static constexpr std::size_t MASK = Capacity - 1;

    // -----------------------------------------------------------------------
    // ProducerState — everything the producer thread touches frequently.
    // Isolated to its own cache line so consumer writes don't invalidate it.
    // -----------------------------------------------------------------------
    struct alignas(64) ProducerState {
        // head_ is the NEXT slot the producer will write into.
        // Only the producer ever writes this; consumer only reads it.
        std::atomic<std::size_t> head{0};

        // Locally cached copy of tail_ (the consumer's pointer).
        // The producer reads the real tail_ only when its cache is stale.
        // This avoids hammering the consumer's cache line on every push.
        std::size_t tail_cache{0};

        // Pad to exactly 64 bytes.
        // sizeof(atomic<size_t>) == 8, sizeof(size_t) == 8 → used 16 B → 48 B pad
        char _pad[64 - sizeof(std::atomic<std::size_t>) - sizeof(std::size_t)];
    };

    // -----------------------------------------------------------------------
    // ConsumerState — everything the consumer thread touches frequently.
    // Isolated to its own cache line so producer writes don't invalidate it.
    // -----------------------------------------------------------------------
    struct alignas(64) ConsumerState {
        // tail_ is the NEXT slot the consumer will read from.
        // Only the consumer ever writes this; producer only reads it.
        std::atomic<std::size_t> tail{0};

        // Locally cached copy of head_ (the producer's pointer).
        std::size_t head_cache{0};

        char _pad[64 - sizeof(std::atomic<std::size_t>) - sizeof(std::size_t)];
    };

    // -----------------------------------------------------------------------
    // Data storage — aligned to 64 bytes so the first slot starts on a
    // cache line boundary. Each slot is sizeof(T) bytes.
    // For Order (64 B), buffer_[i] IS exactly one cache line.
    // -----------------------------------------------------------------------
    alignas(64) T buffer_[Capacity];

    // Producer and consumer state on their own cache lines.
    // Order matters: put buffer_ first to avoid struct padding surprises.
    ProducerState producer_;
    ConsumerState consumer_;

public:
    LockFreeRingBuffer() = default;

    // Non-copyable / non-movable: contains atomics and raw buffer pointer
    LockFreeRingBuffer(const LockFreeRingBuffer&)            = delete;
    LockFreeRingBuffer& operator=(const LockFreeRingBuffer&) = delete;

    // -----------------------------------------------------------------------
    // push — called ONLY by the producer thread
    //
    // Returns true  if item was written to the buffer.
    // Returns false if the buffer is full (caller should retry or back off).
    //
    // noexcept: zero heap allocation, no locks, no exceptions possible.
    // -----------------------------------------------------------------------
    [[nodiscard]] bool push(const T& item) noexcept {
        // 1. Read our own head (relaxed: only we write head).
        const std::size_t head = producer_.head.load(std::memory_order_relaxed);

        // Next slot index (wraps via bitmask).
        const std::size_t next = (head + 1) & MASK;

        // 2. Check if full using cached tail.
        if (next == producer_.tail_cache) {
            // Our cached copy says we're full. Refresh from the real tail_.
            // ACQUIRE: ensures we see all consumer writes that happened before
            // the consumer's RELEASE store to tail_.
            producer_.tail_cache = consumer_.tail.load(std::memory_order_acquire);

            if (next == producer_.tail_cache) {
                return false; // genuinely full — caller must retry
            }
        }

        // 3. Write data into the slot. Plain assignment is fine because:
        //    • Only the producer writes this slot at this moment.
        //    • The following RELEASE store to head_ creates the happens-before
        //      edge that makes this write visible to the consumer.
        buffer_[head] = item;

        // 4. Publish new head with RELEASE so the consumer can safely read
        //    buffer_[head] after seeing head_ advanced past it.
        producer_.head.store(next, std::memory_order_release);

        return true;
    }

    // -----------------------------------------------------------------------
    // pop — called ONLY by the consumer thread
    //
    // Returns true  if an item was read into `item`.
    // Returns false if the buffer is empty.
    // -----------------------------------------------------------------------
    [[nodiscard]] bool pop(T& item) noexcept {
        // 1. Read our own tail (relaxed: only we write tail).
        const std::size_t tail = consumer_.tail.load(std::memory_order_relaxed);

        // 2. Check if empty using cached head.
        if (tail == consumer_.head_cache) {
            // Our cached copy says we're empty. Refresh from the real head_.
            // ACQUIRE: ensures we see the data written before the producer's
            // RELEASE store to head_.
            consumer_.head_cache = producer_.head.load(std::memory_order_acquire);

            if (tail == consumer_.head_cache) {
                return false; // genuinely empty
            }
        }

        // 3. Copy the data out of the slot.
        item = buffer_[tail];

        // 4. Advance tail with RELEASE so the producer can reuse this slot.
        consumer_.tail.store((tail + 1) & MASK, std::memory_order_release);

        return true;
    }

    // -----------------------------------------------------------------------
    // Capacity / occupancy queries (approximate — values may be stale)
    // -----------------------------------------------------------------------
    [[nodiscard]] std::size_t capacity()     const noexcept { return Capacity; }

    [[nodiscard]] std::size_t size_approx()  const noexcept {
        const std::size_t h = producer_.head.load(std::memory_order_relaxed);
        const std::size_t t = consumer_.tail.load(std::memory_order_relaxed);
        return (h - t) & MASK;
    }

    [[nodiscard]] bool empty_approx() const noexcept {
        return producer_.head.load(std::memory_order_relaxed) ==
               consumer_.tail.load(std::memory_order_relaxed);
    }
};
