#pragma once
// =============================================================================
// latency_tracker.h  —  Stage 6: Lock-Free Latency Measurement
// =============================================================================
//
// CONCEPT: High-Resolution Timing
//
// std::chrono::high_resolution_clock is the finest-granularity clock available
// on the platform. On Linux x86-64 it maps to CLOCK_MONOTONIC_RAW which reads
// the TSC (Time Stamp Counter) — a 64-bit register that increments at the CPU
// frequency (~3-4 GHz). Reading the TSC takes ~3-10 ns.
//
// In HFT we measure latency as:
//   ingress_ns  — when the order entered the system (stamped in market_feed)
//   egress_ns   — when the order exited the risk engine (stamped in main)
//   latency     = egress_ns - ingress_ns
//
// CONCEPT: Lock-Free Statistics Accumulation
//
// We want to update min/max/count/sum from the consumer thread without
// locking. We use:
//   • fetch_add for count and sum (always safe — commutative, associative)
//   • CAS loops for min and max (load → compare → CAS, retry on conflict)
//
// CONCEPT: Power-of-2 Histogram Buckets
//
// Maintaining a sorted array for exact percentiles is expensive. Instead we
// use a log2 histogram: bucket[k] counts latencies in [2^k, 2^(k+1)) ns.
//   bucket[0]  = [1, 2) ns      (impossible on real hardware but slot exists)
//   bucket[10] = [1024, 2048) ns  ≈ [1 μs, 2 μs)
//   bucket[20] = [1M, 2M) ns     ≈ [1 ms, 2 ms)
//
// This gives us a rough percentile distribution with zero allocation and
// lock-free updates (one atomic increment per sample).
// =============================================================================

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <climits>

// ---------------------------------------------------------------------------
// LatencyTracker — accumulates nanosecond latency samples, lock-free.
// ---------------------------------------------------------------------------
class LatencyTracker {
public:
    // Number of histogram buckets.
    // Bucket k covers [2^k, 2^(k+1)) nanoseconds.
    // 64 buckets covers up to 2^64 ns ≈ 584 years — more than enough.
    static constexpr int NUM_BUCKETS = 64;

private:
    // -----------------------------------------------------------------------
    // Hot path statistics — each on its own cache line to avoid false sharing.
    // The consumer thread updates count_ and sum_ at every sample.
    // -----------------------------------------------------------------------
    struct alignas(64) HotStats {
        std::atomic<uint64_t> count{0};
        std::atomic<uint64_t> sum_ns{0};
        char _pad[64 - 2 * sizeof(std::atomic<uint64_t>)];
    } hot_;

    struct alignas(64) MinMax {
        std::atomic<uint64_t> min_ns{UINT64_MAX};
        std::atomic<uint64_t> max_ns{0};
        char _pad[64 - 2 * sizeof(std::atomic<uint64_t>)];
    } minmax_;

    // -----------------------------------------------------------------------
    // Histogram — one atomic counter per log2 bucket.
    // Each counter fits on its own sub-cache-line naturally (8 bytes × 8 = 64).
    // -----------------------------------------------------------------------
    alignas(64) std::atomic<uint64_t> histogram_[NUM_BUCKETS]{};

    // -----------------------------------------------------------------------
    // Compute floor(log2(v)) efficiently using a builtin or manual fallback.
    // -----------------------------------------------------------------------
    static int log2_floor(uint64_t v) noexcept {
        if (v == 0) return 0;
#if defined(__GNUC__) || defined(__clang__)
        // __builtin_clzll: count leading zeros in unsigned long long
        // log2(v) = 63 - clz(v)  (for v > 0)
        return 63 - __builtin_clzll(v);
#else
        int bit = 0;
        while (v >>= 1) ++bit;
        return bit;
#endif
    }

public:
    LatencyTracker() = default;

    // Non-copyable: contains atomics
    LatencyTracker(const LatencyTracker&)            = delete;
    LatencyTracker& operator=(const LatencyTracker&) = delete;

    // -----------------------------------------------------------------------
    // record() — add one latency sample. Called on the hot path.
    //
    // Operations are relaxed where possible (min/max use CAS).
    // Relaxed stores are fine for the histogram and sum/count because we only
    // read the final results once after all threads have stopped — there is no
    // synchronisation requirement during accumulation.
    // -----------------------------------------------------------------------
    void record(uint64_t latency_ns) noexcept {
        // count and sum: fetch_add is a single atomic RMW — no CAS needed
        hot_.count.fetch_add(1,           std::memory_order_relaxed);
        hot_.sum_ns.fetch_add(latency_ns, std::memory_order_relaxed);

        // min: CAS loop (compare_exchange_weak spins only on actual contention)
        uint64_t cur_min = minmax_.min_ns.load(std::memory_order_relaxed);
        while (latency_ns < cur_min &&
               !minmax_.min_ns.compare_exchange_weak(
                   cur_min, latency_ns,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed))
        { /* cur_min refreshed by CAS */ }

        // max: same pattern
        uint64_t cur_max = minmax_.max_ns.load(std::memory_order_relaxed);
        while (latency_ns > cur_max &&
               !minmax_.max_ns.compare_exchange_weak(
                   cur_max, latency_ns,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed))
        { /* cur_max refreshed by CAS */ }

        // histogram: find the bucket and increment it
        const int bucket = std::min(log2_floor(latency_ns), NUM_BUCKETS - 1);
        histogram_[bucket].fetch_add(1, std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------------
    // Accessors — call after all producer threads have finished
    // -----------------------------------------------------------------------
    [[nodiscard]] uint64_t count()   const noexcept {
        return hot_.count.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t sum_ns()  const noexcept {
        return hot_.sum_ns.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t min_ns()  const noexcept {
        const uint64_t v = minmax_.min_ns.load(std::memory_order_relaxed);
        return (v == UINT64_MAX) ? 0 : v;
    }
    [[nodiscard]] uint64_t max_ns()  const noexcept {
        return minmax_.max_ns.load(std::memory_order_relaxed);
    }
    [[nodiscard]] double avg_ns()    const noexcept {
        const uint64_t n = count();
        return n ? static_cast<double>(sum_ns()) / n : 0.0;
    }

    // -----------------------------------------------------------------------
    // percentile() — approximate percentile from log2 histogram.
    //
    // Returns the upper bound of the bucket that contains the p-th percentile.
    // E.g. percentile(0.99) ≈ 99th percentile latency.
    // -----------------------------------------------------------------------
    [[nodiscard]] uint64_t percentile(double p) const noexcept {
        const uint64_t n = count();
        if (n == 0 || p <= 0.0) return 0;
        if (p >= 1.0) return max_ns();

        uint64_t target = static_cast<uint64_t>(p * static_cast<double>(n));
        uint64_t cumulative = 0;

        for (int i = 0; i < NUM_BUCKETS; ++i) {
            cumulative += histogram_[i].load(std::memory_order_relaxed);
            if (cumulative >= target) {
                // Return the upper bound of bucket i: 2^(i+1) - 1 ns
                return (i < 63) ? (UINT64_C(2) << i) - 1 : UINT64_MAX;
            }
        }
        return max_ns();
    }

    // -----------------------------------------------------------------------
    // print_report() — human-readable latency summary to stdout
    // -----------------------------------------------------------------------
    void print_report(const char* label = "Latency") const noexcept {
        const uint64_t n = count();
        if (n == 0) {
            std::printf("[%s] no samples recorded\n", label);
            return;
        }
        std::printf("\n=== %s Report (%llu samples) ===\n",
                    label, static_cast<unsigned long long>(n));
        std::printf("  Min      : %7llu ns\n",
                    static_cast<unsigned long long>(min_ns()));
        std::printf("  Avg      : %7.1f ns\n",  avg_ns());
        std::printf("  p50      : %7llu ns\n",
                    static_cast<unsigned long long>(percentile(0.50)));
        std::printf("  p90      : %7llu ns\n",
                    static_cast<unsigned long long>(percentile(0.90)));
        std::printf("  p99      : %7llu ns\n",
                    static_cast<unsigned long long>(percentile(0.99)));
        std::printf("  p99.9    : %7llu ns\n",
                    static_cast<unsigned long long>(percentile(0.999)));
        std::printf("  Max      : %7llu ns\n",
                    static_cast<unsigned long long>(max_ns()));

        // Print non-empty histogram buckets
        std::printf("\n  Histogram (log2 buckets):\n");
        for (int i = 0; i < NUM_BUCKETS; ++i) {
            const uint64_t cnt = histogram_[i].load(std::memory_order_relaxed);
            if (cnt == 0) continue;
            const uint64_t lo = (i > 0) ? (UINT64_C(1) << i) : 0;
            const uint64_t hi = (i < 63) ? (UINT64_C(2) << i) - 1 : UINT64_MAX;
            std::printf("    [%6llu - %6llu ns] : %llu\n",
                        static_cast<unsigned long long>(lo),
                        static_cast<unsigned long long>(hi),
                        static_cast<unsigned long long>(cnt));
        }
        std::printf("\n");
    }
};
