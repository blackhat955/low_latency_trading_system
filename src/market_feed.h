#pragma once
// =============================================================================
// market_feed.h  —  Stage 1 & 2: Market Data Feed Producer
// =============================================================================
//
// CONCEPT: Producer-Consumer Pattern
//
// The market feed thread is a high-frequency producer. Real feeds (CME MDP 3.0,
// OPRA, ITCH) deliver millions of messages per second over UDP multicast.
// Our simulation generates synthetic orders as fast as possible to stress-test
// the pipeline.
//
// Key design decisions:
//   • Spin-wait on full buffer with exponential backoff to avoid monopolising
//     the CPU while still being reactive (better latency than sleep_for).
//   • Symbol hashes are computed once at startup; the hot path uses the integer
//     hash for O(1) symbol lookup instead of string comparison.
//   • All state needed per order is filled in a local variable, then pushed
//     into the ring buffer with a single copy — no heap allocation.
//
// CONCEPT: Backpressure
//
// If the consumer is slower than the producer the ring buffer fills up. The
// producer must either:
//   (a) Block — easiest but adds latency
//   (b) Drop — fastest but loses data (used for market data updates where
//              the newest price supersedes the old one)
//   (c) Backpressure upstream — signal the feed to slow down
//
// We implement (a) with spin + yield for a learning demo.
// =============================================================================

#include "order.h"
#include "ring_buffer.h"
#include "latency_tracker.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <random>
#include <string_view>
#include <thread>

// ---------------------------------------------------------------------------
// Instrument — precomputed per-symbol data to avoid hot-path allocations
// ---------------------------------------------------------------------------
struct Instrument {
    const char* name;         // e.g. "AAPL"
    uint64_t    hash;         // std::hash<std::string_view> of name
    double      base_price;   // reference price for random walk simulation
};

// ---------------------------------------------------------------------------
// MarketFeed
//
// Simulates a high-frequency market data feed.
// Runs on its own thread, pushing Order objects into a LockFreeRingBuffer.
// ---------------------------------------------------------------------------
template<std::size_t RingCapacity>
class MarketFeed {

    using RingBuf = LockFreeRingBuffer<Order, RingCapacity>;

    RingBuf&                    ring_;      // reference to the shared ring buffer
    std::atomic<bool>&          running_;   // shared stop flag
    LatencyTracker&             tracker_;   // latency tracker (records ingress time)
    uint64_t                    max_orders_;// 0 = unlimited

    // Precomputed instrument table (avoids hash computation on hot path)
    static constexpr Instrument INSTRUMENTS[] = {
        { "AAPL",  0, 175.00 },
        { "GOOG",  0, 140.00 },
        { "MSFT",  0, 415.00 },
        { "AMZN",  0, 185.00 },
        { "TSLA",  0,  175.00 },
        { "NVDA",  0, 875.00 },
        { "META",  0, 510.00 },
        { "AMD",   0, 165.00 },
    };
    static constexpr std::size_t NUM_INSTRUMENTS =
        sizeof(INSTRUMENTS) / sizeof(INSTRUMENTS[0]);

    // Cache the hashes at construction time (called once, off the hot path)
    uint64_t hashes_[NUM_INSTRUMENTS]{};

    void init_hashes() noexcept {
        std::hash<std::string_view> hasher;
        for (std::size_t i = 0; i < NUM_INSTRUMENTS; ++i) {
            hashes_[i] = hasher(INSTRUMENTS[i].name);
        }
    }

public:
    MarketFeed(RingBuf& ring,
               std::atomic<bool>& running,
               LatencyTracker& tracker,
               uint64_t max_orders = 0) noexcept
        : ring_(ring), running_(running),
          tracker_(tracker), max_orders_(max_orders)
    {
        init_hashes();
    }

    // -----------------------------------------------------------------------
    // run() — main loop, called from a std::thread
    //
    // Generates synthetic orders and pushes them into the ring buffer as fast
    // as possible. Stops when running_ is set to false or max_orders_ reached.
    // -----------------------------------------------------------------------
    void run() noexcept {
        // Use a fast 64-bit Mersenne Twister.
        // Seed with a fixed value for reproducible benchmarks.
        std::mt19937_64 rng(42);

        // Uniform distributions for each field
        std::uniform_int_distribution<int> sym_dist(
            0, static_cast<int>(NUM_INSTRUMENTS - 1));
        std::uniform_int_distribution<int> qty_dist(1, 500);
        std::uniform_real_distribution<double> price_offset(-5.0, 5.0);
        std::uniform_int_distribution<int> side_dist(0, 1);

        uint64_t order_id    = 1;
        uint64_t spin_count  = 0;   // backpressure spin counter

        while (running_.load(std::memory_order_relaxed)) {
            if (max_orders_ && order_id > max_orders_) {
                break;
            }

            // -------- Build the order (all stack-allocated, no heap) --------
            Order order{};
            const int sym_idx = sym_dist(rng);

            order.order_id     = order_id++;
            order.symbol_hash  = hashes_[sym_idx];
            order.price        = INSTRUMENTS[sym_idx].base_price
                                 + price_offset(rng);
            order.quantity     = static_cast<double>(qty_dist(rng));
            order.side         = static_cast<Side>(side_dist(rng));
            order.status       = OrderStatus::Pending;

            // Copy symbol name (bounded, no strlen on hot path)
            std::memcpy(order.symbol, INSTRUMENTS[sym_idx].name, 5);

            // Stamp creation time — this is T0 for end-to-end latency
            order.timestamp_ns = now_ns();

            // -------- Push into ring buffer with backpressure ---------------
            while (!ring_.push(order)) {
                // Buffer full: apply exponential backoff before retrying.
                // First few spins: pure busy-wait (cheapest on lightly loaded system).
                // After ~1000 spins: yield the thread to avoid starving the consumer.
                ++spin_count;
                if (spin_count > 1000) {
                    std::this_thread::yield();
                    spin_count = 0;
                }

                if (!running_.load(std::memory_order_relaxed)) return;
            }

            // Stamp ingress time — T1, when the item entered the buffer
            // (Note: we can't stamp inside push() without modifying Order after
            //  push; in a real system we'd timestamp before push or use a probe)
            spin_count = 0; // reset after successful push
        }
    }
};

// Out-of-class definition of the constexpr member array
template<std::size_t N>
constexpr Instrument MarketFeed<N>::INSTRUMENTS[];
