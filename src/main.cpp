// =============================================================================
// main.cpp  —  Lock-Free Market Data Engine  ──  Interactive Dashboard
// =============================================================================
//
// Launches the full SPSC pipeline and renders a live terminal dashboard:
//
//   MarketFeed thread  →  LockFreeRingBuffer  →  OrderProcessor thread
//                                  ↓
//                            RiskEngine  →  LatencyTracker  →  Dashboard
//
// Press Ctrl+C to stop cleanly. The terminal is restored on exit.
// =============================================================================

#include "order.h"
#include "ring_buffer.h"
#include "memory_pool.h"
#include "risk_engine.h"
#include "market_feed.h"
#include "latency_tracker.h"
#include "dashboard.h"

#include <atomic>
#include <thread>
#include <chrono>
#include <cstdio>
#include <csignal>

// =============================================================================
// Configuration
// =============================================================================
static constexpr std::size_t RING_CAPACITY = 4096;  // must be power-of-2
static constexpr std::size_t POOL_SIZE     = 8192;  // memory pool slots

// Global stop flag — set by SIGINT handler (Ctrl+C)
static std::atomic<bool> g_running{true};

static void sigint_handler(int) noexcept {
    g_running.store(false, std::memory_order_relaxed);
}

// =============================================================================
// OrderProcessor — consumer thread
// Pops orders from the ring, runs risk checks, records latency and recent feed.
// =============================================================================
class OrderProcessor {
    LockFreeRingBuffer<Order, RING_CAPACITY>& ring_;
    std::atomic<bool>&                        running_;
    RiskEngine&                               risk_;
    LatencyTracker&                           tracker_;
    RecentOrdersBuffer&                       recent_;
    std::atomic<uint64_t>&                    processed_;

public:
    OrderProcessor(LockFreeRingBuffer<Order, RING_CAPACITY>& ring,
                   std::atomic<bool>&   running,
                   RiskEngine&          risk,
                   LatencyTracker&      tracker,
                   RecentOrdersBuffer&  recent,
                   std::atomic<uint64_t>& processed) noexcept
        : ring_(ring), running_(running), risk_(risk),
          tracker_(tracker), recent_(recent), processed_(processed) {}

    void run() noexcept {
        Order order{};
        uint64_t spins = 0;

        while (running_.load(std::memory_order_relaxed) || !ring_.empty_approx()) {
            if (!ring_.pop(order)) {
                if (++spins > 500) { std::this_thread::yield(); spins = 0; }
                continue;
            }
            spins = 0;

            // Stamp egress immediately for accurate latency measurement
            const uint64_t egress_ns = now_ns();

            // Risk check (atomic CAS position update)
            const bool accepted = risk_.check_and_accept(order);
            (void)accepted;

            // Record end-to-end latency: T1(egress) - T0(timestamp in feed)
            uint64_t latency = 0;
            if (egress_ns > order.timestamp_ns) {
                latency = egress_ns - order.timestamp_ns;
                tracker_.record(latency);
            }

            // Push snapshot to the display feed
            recent_.push(order, latency);

            processed_.fetch_add(1, std::memory_order_relaxed);
        }
    }
};

// =============================================================================
// main
// =============================================================================
int main() {
    // Install Ctrl+C handler
    std::signal(SIGINT, sigint_handler);

    // -------------------------------------------------------------------------
    // Shared state — all on the stack, no heap allocation after startup
    // -------------------------------------------------------------------------
    LockFreeRingBuffer<Order, RING_CAPACITY> ring;
    MemoryPool<Order, POOL_SIZE>             pool;

    RiskLimits limits;
    limits.max_position  = 50'000;
    limits.max_order_qty =    500;
    limits.max_notional  = 500'000.0;
    RiskEngine     risk(limits);

    LatencyTracker    tracker;
    RecentOrdersBuffer recent;

    std::atomic<uint64_t> processed{0};

    // -------------------------------------------------------------------------
    // Dashboard (main thread renders this)
    // -------------------------------------------------------------------------
    using Dash = Dashboard<RING_CAPACITY, POOL_SIZE>;
    Dash dashboard(processed, ring, risk, tracker, recent, pool, g_running);

    Dash::init_terminal();

    // -------------------------------------------------------------------------
    // Launch producer thread
    // -------------------------------------------------------------------------
    MarketFeed<RING_CAPACITY> feed(ring, g_running, tracker, /*max_orders=*/0);

    std::thread producer([&feed]() noexcept { feed.run(); });

    // -------------------------------------------------------------------------
    // Launch consumer thread
    // -------------------------------------------------------------------------
    OrderProcessor processor(ring, g_running, risk, tracker, recent, processed);

    std::thread consumer([&processor]() noexcept { processor.run(); });

    // -------------------------------------------------------------------------
    // Main thread: dashboard render loop
    //
    // We render every 100 ms (10 fps) — fast enough to look live,
    // slow enough to avoid wasting CPU on the display thread.
    // Throughput is sampled once per second for the sparkline.
    // -------------------------------------------------------------------------
    auto last_spark = std::chrono::steady_clock::now();

    while (g_running.load(std::memory_order_relaxed)) {
        // Sample throughput once per second
        auto now_tp = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now_tp - last_spark).count() >= 1.0) {
            dashboard.sample_throughput();
            last_spark = now_tp;
        }

        dashboard.render();

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // -------------------------------------------------------------------------
    // Shutdown: drain the ring, join threads, print final report
    // -------------------------------------------------------------------------
    g_running.store(false, std::memory_order_relaxed);
    producer.join();
    consumer.join();

    // One final render to show terminal state
    dashboard.sample_throughput();
    dashboard.render();

    Dash::restore_terminal();

    // Print a summary below the dashboard box
    const uint64_t n = processed.load(std::memory_order_relaxed);
    std::printf("\n  Engine stopped.\n");
    std::printf("  Total orders processed : %llu\n",
                static_cast<unsigned long long>(n));
    std::printf("  Accepted               : %llu\n",
                static_cast<unsigned long long>(risk.orders_accepted()));
    std::printf("  Rejected               : %llu\n",
                static_cast<unsigned long long>(risk.orders_rejected()));
    tracker.print_report("Final Latency");

    return 0;
}
