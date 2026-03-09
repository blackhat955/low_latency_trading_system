#pragma once
// =============================================================================
// risk_engine.h  —  Stage 5: Lock-Free Risk Engine
// =============================================================================
//
// CONCEPT: Per-Symbol Atomic Position Tracking
//
// In a real trading system a risk engine checks every order before it is sent
// to the exchange. Common checks include:
//   • Maximum order size (protect against fat-finger errors)
//   • Maximum notional value (price × quantity)
//   • Net position limits per symbol (don't go too long/short)
//   • Daily loss limits (halt if P&L drops below threshold)
//
// CONCEPT: False Sharing in Arrays of Atomics
//
// If we stored positions as `std::atomic<int64_t> positions_[N]` naively, two
// threads updating adjacent symbols could share a cache line, causing false
// sharing that serialises the updates even though they are logically independent.
//
// Solution: wrap each atomic in a struct with explicit padding to 64 bytes,
// then use an array of those padded structs. Each symbol's state lives on its
// own cache line, so updates to different symbols never contend.
//
// CONCEPT: Check-Then-Act Race
//
// In a concurrent system, checking a limit and then updating the position is
// NOT atomic. Between the check and the update another thread could push the
// position over the limit. Two approaches:
//   1. Full CAS loop: read, check, CAS — retry until success or reject.
//   2. Optimistic update: add delta, check result, subtract back if bad.
//
// We use approach (1) here for correctness under concurrent producers.
// =============================================================================

#include "order.h"
#include <atomic>
#include <array>
#include <cstdint>
#include <cstdlib>   // std::abs
#include <cmath>     // std::isnan, std::isinf

// ---------------------------------------------------------------------------
// RiskLimits — tunable limits (could be loaded from config at startup)
// ---------------------------------------------------------------------------
struct RiskLimits {
    int64_t max_position   = 10'000;    // max net shares per symbol
    int64_t max_order_qty  =  1'000;    // max qty in a single order
    double  max_notional   = 5'000'000; // max price × qty in one order (USD)
    double  max_daily_loss = 1'000'000; // halt if realised loss exceeds this
};

// ---------------------------------------------------------------------------
// RiskEngine
// ---------------------------------------------------------------------------
class RiskEngine {

    // -----------------------------------------------------------------------
    // SymbolState — all per-symbol risk state, padded to one cache line.
    //
    // The padding means symbol[0] and symbol[1] are on different cache lines.
    // A thread updating "AAPL" and a thread updating "GOOG" never touch the
    // same cache line, so there is zero false sharing between symbols.
    // -----------------------------------------------------------------------
    struct alignas(64) SymbolState {
        std::atomic<int64_t> net_position{0};   // + = long, - = short   (8 B)
        std::atomic<int64_t> gross_volume{0};   // total shares traded    (8 B)
        char _pad[64 - 2 * sizeof(std::atomic<int64_t>)];  // = 48 B
    };

    static_assert(sizeof(SymbolState) == 64, "SymbolState must be 64 bytes");

    // -----------------------------------------------------------------------
    // Engine-wide daily P&L (shared, on its own cache line)
    // -----------------------------------------------------------------------
    struct alignas(64) DailyState {
        std::atomic<int64_t> realised_pnl_cents{0}; // in integer cents
        std::atomic<uint64_t> orders_checked{0};
        std::atomic<uint64_t> orders_rejected{0};
        char _pad[64 - 3 * sizeof(std::atomic<int64_t>)];
    };

    // -----------------------------------------------------------------------
    // Storage
    // -----------------------------------------------------------------------
    static constexpr std::size_t MAX_SYMBOLS = 64; // power of 2 for cheap modulo

    std::array<SymbolState, MAX_SYMBOLS> symbols_;
    DailyState daily_;
    RiskLimits limits_;

    // Map a symbol_hash to a slot in symbols_[]
    static std::size_t slot(uint64_t symbol_hash) noexcept {
        return symbol_hash & (MAX_SYMBOLS - 1);
    }

public:
    explicit RiskEngine(const RiskLimits& limits = RiskLimits{}) noexcept
        : limits_(limits) {}

    // Non-copyable: contains atomic arrays
    RiskEngine(const RiskEngine&)            = delete;
    RiskEngine& operator=(const RiskEngine&) = delete;

    // -----------------------------------------------------------------------
    // check_and_accept()
    //
    // Performs all risk checks on `order`. If all pass, atomically updates
    // the position and returns true (order accepted). Otherwise returns false.
    //
    // Thread-safe: multiple threads may call this concurrently for different
    // or even the same symbol (CAS loop handles contention).
    // -----------------------------------------------------------------------
    [[nodiscard]] bool check_and_accept(Order& order) noexcept {
        daily_.orders_checked.fetch_add(1, std::memory_order_relaxed);

        // --- 1. Sanity checks (fast, no atomics) ----------------------------
        if (order.quantity <= 0.0 || std::isnan(order.price) ||
            std::isinf(order.price) || order.price <= 0.0)
        {
            return reject(order);
        }

        // --- 2. Order size limit --------------------------------------------
        if (static_cast<int64_t>(order.quantity) > limits_.max_order_qty) {
            return reject(order);
        }

        // --- 3. Notional limit ----------------------------------------------
        if (order.price * order.quantity > limits_.max_notional) {
            return reject(order);
        }

        // --- 4. Position limit — CAS loop -----------------------------------
        // We need to atomically: check current_pos + delta ≤ max, then apply.
        const std::size_t s = slot(order.symbol_hash);
        const int64_t delta = (order.side == Side::Buy)
                              ? static_cast<int64_t>(order.quantity)
                              : -static_cast<int64_t>(order.quantity);

        int64_t current = symbols_[s].net_position.load(std::memory_order_relaxed);

        while (true) {
            const int64_t new_pos = current + delta;

            // Reject if new position would exceed limit
            if (std::abs(new_pos) > limits_.max_position) {
                return reject(order);
            }

            // Try to atomically apply the delta.
            // On failure `current` is refreshed with the latest value → retry.
            if (symbols_[s].net_position.compare_exchange_weak(
                    current, new_pos,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                break; // successfully applied
            }
            // Retry (another thread updated the position concurrently)
        }

        // Update gross volume (relaxed — only used for analytics, not safety)
        symbols_[s].gross_volume.fetch_add(
            static_cast<int64_t>(order.quantity),
            std::memory_order_relaxed);

        order.status = OrderStatus::Accepted;
        return true;
    }

    // -----------------------------------------------------------------------
    // Diagnostics
    // -----------------------------------------------------------------------
    int64_t net_position(uint64_t symbol_hash) const noexcept {
        return symbols_[slot(symbol_hash)].net_position.load(std::memory_order_relaxed);
    }

    uint64_t orders_checked()  const noexcept {
        return daily_.orders_checked.load(std::memory_order_relaxed);
    }
    uint64_t orders_rejected() const noexcept {
        return daily_.orders_rejected.load(std::memory_order_relaxed);
    }
    uint64_t orders_accepted() const noexcept {
        const uint64_t chk = orders_checked();
        const uint64_t rej = orders_rejected();
        return chk > rej ? chk - rej : 0;
    }

    void print_stats() const noexcept;  // defined after includes in main.cpp

private:
    bool reject(Order& order) noexcept {
        order.status = OrderStatus::Rejected;
        daily_.orders_rejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
};
