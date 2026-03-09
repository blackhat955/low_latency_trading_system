#pragma once
// =============================================================================
// dashboard.h — Real-time ANSI Terminal Dashboard for the Market Data Engine
// =============================================================================
//
// Renders a live, updating TUI using ANSI escape codes and UTF-8 box-drawing.
// No external libraries required — pure POSIX / libc.
//
// Architecture:
//   • Main thread calls dashboard.render() every ~100 ms
//   • Consumer thread calls recent_.push() after each order
//   • Dashboard reads all shared state with relaxed atomics (display-only)
//   • Terminal cursor is repositioned to top-left on each render (no flicker)
// =============================================================================

#include "order.h"
#include "ring_buffer.h"
#include "risk_engine.h"
#include "latency_tracker.h"
#include "memory_pool.h"

#include <atomic>
#include <array>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdarg>

#if defined(__unix__) || defined(__APPLE__)
#  include <sys/ioctl.h>
#  include <unistd.h>
#endif

// =============================================================================
// ANSI escape code helpers
// =============================================================================
namespace ansi {
    constexpr const char* RESET   = "\033[0m";
    constexpr const char* BOLD    = "\033[1m";
    constexpr const char* RED     = "\033[31m";
    constexpr const char* GREEN   = "\033[32m";
    constexpr const char* YELLOW  = "\033[33m";
    constexpr const char* CYAN    = "\033[36m";
    constexpr const char* GRAY    = "\033[90m";
    constexpr const char* BGREEN  = "\033[92m";
    constexpr const char* BRED    = "\033[91m";
    constexpr const char* BYELLOW = "\033[93m";
    constexpr const char* BCYAN   = "\033[96m";
    constexpr const char* BWHITE  = "\033[97m";

    constexpr const char* HOME        = "\033[H";
    constexpr const char* CLEAR       = "\033[2J";
    constexpr const char* HIDE_CURSOR = "\033[?25l";
    constexpr const char* SHOW_CURSOR = "\033[?25h";
}

// Unicode sparkline blocks (UTF-8 encoded) — each renders as 1 visible char
static const char* const SPARK[] = {" ","▁","▂","▃","▄","▅","▆","▇","█"};

// =============================================================================
// Layout constants — everything based on an 80-column terminal
//
// Line layout:  ║ + CONTENT(78) + ║
// Two-column:   ║ + SP + L(36) + SP + │ + SP + R(37) + SP + ║
//                 1    1    36    1   1   1    37    1    1  = 80 ✓
// Full-width:   ║ + SP + CONTENT(76) + SP + ║   = 80 ✓
// =============================================================================
static constexpr int BOX_INNER = 78; // chars between the two ║ chars
static constexpr int L_COL    = 36;  // visible width of left column
static constexpr int R_COL    = 37;  // visible width of right column
static constexpr int FULL_VW  = 76;  // visible width of full-width content
static constexpr int L_SEP    = 38;  // ═ count on left side of ╠...╩...╣ line
static constexpr int R_SEP    = 39;  // ═ count on right side

// =============================================================================
// Visible-character length of an ANSI-decorated UTF-8 string
// ANSI escape sequences (\033[...m) are zero-width.
// UTF-8 multi-byte sequences count as one visible character each.
// =============================================================================
static int vlen(const char* s) noexcept {
    int n = 0;
    while (*s) {
        if (*s == '\033') {                   // skip ANSI sequence \033[...m
            while (*s && *s != 'm') ++s;
            if (*s) ++s;
        } else {
            unsigned char c = static_cast<unsigned char>(*s);
            // Advance by UTF-8 sequence length; count as 1 visible char
            if      (c < 0x80) { ++s; }
            else if ((c & 0xE0) == 0xC0) { s += 2; }
            else if ((c & 0xF0) == 0xE0) { s += 3; }
            else                          { s += 4; }
            ++n;
        }
    }
    return n;
}

// Print `content` then space-pad so total visible width == target
static void vcell(const char* content, int target) noexcept {
    std::printf("%s", content);
    int pad = target - vlen(content);
    while (pad-- > 0) std::printf(" ");
}

// =============================================================================
// Box-drawing primitives
// =============================================================================

// Full-width horizontal border line: ╔═...═╗ / ╠═...═╣ / ╚═...═╝
static void hline(const char* lc, const char* mid, const char* rc) noexcept {
    std::printf("%s%s", ansi::CYAN, lc);
    for (int i = 0; i < BOX_INNER; ++i) std::printf("%s", mid);
    std::printf("%s%s\n", rc, ansi::RESET);
}

// Split horizontal: ╠═...═╩═...═╣
static void hline_split() noexcept {
    std::printf("%s╠", ansi::CYAN);
    for (int i = 0; i < L_SEP; ++i) std::printf("═");
    std::printf("╩");
    for (int i = 0; i < R_SEP; ++i) std::printf("═");
    std::printf("╣%s\n", ansi::RESET);
}

// Full-width content row: ║ content (padded to FULL_VW) ║
static void full_row(const char* content) noexcept {
    std::printf("%s║%s ", ansi::CYAN, ansi::RESET);
    vcell(content, FULL_VW);
    std::printf(" %s║%s\n", ansi::CYAN, ansi::RESET);
}

// Two-column row: ║ L_COL │ R_COL ║
static void split_row(const char* left, const char* right) noexcept {
    std::printf("%s║%s ", ansi::CYAN, ansi::RESET);
    vcell(left, L_COL);
    std::printf(" %s│%s ", ansi::CYAN, ansi::RESET);
    vcell(right, R_COL);
    std::printf(" %s║%s\n", ansi::CYAN, ansi::RESET);
}

// =============================================================================
// Formatting helpers
// =============================================================================

// Format uint64 with thousands commas into buf
static void fmt_comma(char* buf, int sz, uint64_t n) noexcept {
    char tmp[32];
    std::snprintf(tmp, sizeof(tmp), "%llu", static_cast<unsigned long long>(n));
    int len = static_cast<int>(strlen(tmp));
    int out = 0;
    for (int i = 0; i < len && out < sz - 1; ++i) {
        if (i > 0 && (len - i) % 3 == 0) buf[out++] = ',';
        buf[out++] = tmp[i];
    }
    buf[out] = '\0';
}

// Format nanoseconds as μs or ms
static void fmt_lat(char* buf, int sz, uint64_t ns) noexcept {
    if (ns == 0)               std::snprintf(buf, sz, "      0 ns");
    else if (ns < 1'000)       std::snprintf(buf, sz, " %5llu ns",
                                             static_cast<unsigned long long>(ns));
    else if (ns < 1'000'000)   std::snprintf(buf, sz, " %5.1f μs", ns / 1e3);
    else                       std::snprintf(buf, sz, " %5.2f ms", ns / 1e6);
}

// Format seconds as HH:MM:SS
static void fmt_time(char* buf, int sz, double secs) noexcept {
    auto s = static_cast<uint64_t>(secs);
    std::snprintf(buf, sz, "%02llu:%02llu:%02llu",
                  s / 3600, (s % 3600) / 60, s % 60);
}

// Progress bar (fill_color █, empty ░)
static void draw_bar(double frac, int width,
                     const char* fill  = "\033[32m",
                     const char* empty = "\033[90m") noexcept
{
    frac = std::max(0.0, std::min(1.0, frac));
    int filled = static_cast<int>(frac * width + 0.5);
    std::printf("%s", fill);
    for (int i = 0; i < filled; ++i)      std::printf("█");
    std::printf("%s", empty);
    for (int i = filled; i < width; ++i)  std::printf("░");
    std::printf("%s", ansi::RESET);
}

// =============================================================================
// OrderSnapshot — lightweight display-only copy of a processed order
// =============================================================================
struct OrderSnapshot {
    uint64_t    order_id{0};
    char        symbol[8]{};
    double      price{0.0};
    double      quantity{0.0};
    Side        side{Side::Buy};
    OrderStatus status{OrderStatus::Pending};
    uint64_t    latency_ns{0};
    bool        valid{false};
};

// =============================================================================
// RecentOrdersBuffer — circular buffer for the order feed display
//
// The consumer thread calls push(); the display thread calls read_recent().
// Minor display tearing is acceptable (this is view-only data).
// =============================================================================
class RecentOrdersBuffer {
    static constexpr int SIZE = 8;

    struct Slot {
        OrderSnapshot snap{};
        std::atomic<bool> ready{false};
    };

    std::array<Slot, SIZE> slots_{};
    std::atomic<uint64_t>  write_idx_{0};

public:
    void push(const Order& o, uint64_t latency_ns) noexcept {
        const uint64_t wi  = write_idx_.fetch_add(1, std::memory_order_relaxed);
        Slot& slot = slots_[wi % SIZE];

        slot.ready.store(false, std::memory_order_release);

        slot.snap.order_id   = o.order_id;
        slot.snap.price      = o.price;
        slot.snap.quantity   = o.quantity;
        slot.snap.side       = o.side;
        slot.snap.status     = o.status;
        slot.snap.latency_ns = latency_ns;
        slot.snap.valid      = true;
        std::memcpy(slot.snap.symbol, o.symbol, 7);

        slot.ready.store(true, std::memory_order_release);
    }

    // Fill `out[0..n-1]` with the n most-recent orders (most recent first).
    // Returns the number of entries actually filled.
    int read_recent(OrderSnapshot* out, int n) const noexcept {
        const uint64_t wi = write_idx_.load(std::memory_order_relaxed);
        const int avail   = static_cast<int>(std::min(wi, static_cast<uint64_t>(SIZE)));
        const int count   = std::min(n, avail);

        for (int i = 0; i < count; ++i) {
            const uint64_t idx = (wi - 1 - i) % SIZE;
            const Slot& slot   = slots_[idx];
            if (slot.ready.load(std::memory_order_acquire))
                out[i] = slot.snap;
            else
                out[i].valid = false;
        }
        return count;
    }
};

// =============================================================================
// Dashboard<RingCapacity, PoolSize>
// =============================================================================
template<std::size_t RingCapacity, std::size_t PoolSize>
class Dashboard {
protected:

    static constexpr int SPARK_LEN = 58; // number of sparkline history buckets

    // References to live engine state (all reads are display-only)
    std::atomic<uint64_t>&                  processed_;
    LockFreeRingBuffer<Order, RingCapacity>& ring_;
    RiskEngine&                              risk_;
    LatencyTracker&                          tracker_;
    RecentOrdersBuffer&                      recent_;
    MemoryPool<Order, PoolSize>&             pool_;
    std::atomic<bool>&                       running_;

    // Throughput sparkline history
    std::array<double, SPARK_LEN> spark_{};
    int      spark_head_{0};       // next write slot in spark_
    uint64_t last_proc_{0};
    uint64_t last_sample_ns_{0};
    double   cur_rate_{0.0};
    double   peak_rate_{0.0};

    uint64_t start_ns_{0};

    // -----------------------------------------------------------------------
    // Section renderers
    // -----------------------------------------------------------------------

    void section_header() noexcept {
        hline("╔", "═", "╗");

        // Title line — build left and right halves
        const char* title = "  LOCK-FREE MARKET DATA ENGINE  ──  LIVE DASHBOARD  ";
        const char* tag   = "  C++17  ";
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "%s%s%s%s%s%s",
                      ansi::BCYAN, ansi::BOLD, title,
                      ansi::RESET, ansi::GRAY, tag);
        full_row(buf);
        hline("╠", "═", "╣");
    }

    void section_status_ring() noexcept {
        const double elapsed_s = static_cast<double>(now_ns() - start_ns_) / 1e9;
        char time_str[16]; fmt_time(time_str, sizeof(time_str), elapsed_s);

        const uint64_t n = processed_.load(std::memory_order_relaxed);
        char n_str[32]; fmt_comma(n_str, sizeof(n_str), n);

        // Rate string
        char rate_str[32];
        if      (cur_rate_ >= 1e6) std::snprintf(rate_str, sizeof(rate_str), "%.2f M/s", cur_rate_ / 1e6);
        else if (cur_rate_ >= 1e3) std::snprintf(rate_str, sizeof(rate_str), "%.1f K/s", cur_rate_ / 1e3);
        else                       std::snprintf(rate_str, sizeof(rate_str), "%.0f /s",  cur_rate_);

        const std::size_t ring_used = ring_.size_approx();
        const double ring_frac = static_cast<double>(ring_used) / RingCapacity;
        const char* ring_clr   = ring_frac < 0.5 ? ansi::BGREEN :
                                 ring_frac < 0.8 ? ansi::BYELLOW : ansi::BRED;
        const bool  is_run     = running_.load(std::memory_order_relaxed);

        char left[256], right[256];

        // Row 0: section headings
        std::snprintf(left,  sizeof(left),  "%sPIPELINE STATUS%s", ansi::BOLD, ansi::RESET);
        std::snprintf(right, sizeof(right), "%sRING BUFFER UTILIZATION%s", ansi::BOLD, ansi::RESET);
        split_row(left, right);

        // Row 1: state | capacity
        std::snprintf(left,  sizeof(left),
                      "  State   :  %s%s● %s%s",
                      is_run ? ansi::BGREEN : ansi::BRED, ansi::BOLD,
                      is_run ? "RUNNING" : "STOPPED",
                      ansi::RESET);
        std::snprintf(right, sizeof(right),
                      "  Capacity : %zu slots  (%zu KB)",
                      RingCapacity, RingCapacity * sizeof(Order) / 1024);
        split_row(left, right);

        // Row 2: runtime | bar
        std::snprintf(left,  sizeof(left),
                      "  Runtime  :  %s%s%s",
                      ansi::YELLOW, time_str, ansi::RESET);
        // Right: build bar inline (bar writes directly to stdout)
        std::printf("%s║%s ", ansi::CYAN, ansi::RESET);
        vcell(left, L_COL);
        std::printf(" %s│%s  Used   : ", ansi::CYAN, ansi::RESET);
        draw_bar(ring_frac, 18, ring_clr);
        char ring_info[64];
        std::snprintf(ring_info, sizeof(ring_info), " %4zu/%zu",
                      ring_used, RingCapacity);
        std::printf("%s", ring_info);
        // pad to R_COL — visible chars so far: "  Used   : " (11) + 18 (bar blocks) + ring_info
        int right_used = 11 + 18 + static_cast<int>(strlen(ring_info));
        int pad = R_COL - right_used;
        while (pad-- > 0) std::printf(" ");
        std::printf(" %s║%s\n", ansi::CYAN, ansi::RESET);

        // Row 3: orders | fill %
        const char* fill_clr = ring_frac > 0.9 ? ansi::BRED : ansi::BGREEN;
        std::snprintf(left,  sizeof(left),
                      "  Orders   :  %s%s%s",
                      ansi::BCYAN, n_str, ansi::RESET);
        std::snprintf(right, sizeof(right),
                      "  Fill     : %s%.1f%%%s",
                      fill_clr, ring_frac * 100.0, ansi::RESET);
        split_row(left, right);

        // Row 4: rate | (blank)
        std::snprintf(left, sizeof(left),
                      "  Rate     :  %s%s%s",
                      ansi::BGREEN, rate_str, ansi::RESET);
        split_row(left, "");

        hline_split();
    }

    void section_sparkline() noexcept {
        // Find max for scaling
        double max_val = 1.0;
        for (double v : spark_) max_val = std::max(max_val, v);

        char peak_str[32];
        if (peak_rate_ >= 1e6) std::snprintf(peak_str, sizeof(peak_str), "%.1fM", peak_rate_ / 1e6);
        else                   std::snprintf(peak_str, sizeof(peak_str), "%.0fK", peak_rate_ / 1e3);

        // Header
        {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "%sTHROUGHPUT HISTORY%s  (orders/s, last %d seconds)",
                          ansi::BOLD, ansi::RESET, SPARK_LEN);
            full_row(buf);
        }

        // Sparkline row: "  peak │ ▁▂▃... │"
        std::printf("%s║%s  %s%-5s%s│ ", ansi::CYAN, ansi::RESET,
                    ansi::YELLOW, peak_str, ansi::RESET);

        for (int i = 0; i < SPARK_LEN; ++i) {
            const int idx  = (spark_head_ + i) % SPARK_LEN;
            const double f = (max_val > 0) ? spark_[idx] / max_val : 0.0;
            if (f < 0.001) {
                std::printf("%s%s", ansi::GRAY, SPARK[0]);
            } else {
                const char* clr = f < 0.5 ? ansi::GREEN :
                                  f < 0.8 ? ansi::BYELLOW : ansi::BRED;
                int blk = static_cast<int>(f * 8.0 + 0.5);
                blk = std::max(1, std::min(8, blk));
                std::printf("%s%s", clr, SPARK[blk]);
            }
        }
        // Visible: 2 + 5 + 1 + 1 + SPARK_LEN = SPARK_LEN + 9
        // Remaining to FULL_VW:
        int rem = FULL_VW - SPARK_LEN - 9;
        std::printf("%s%*s %s║%s\n", ansi::RESET, rem, "", ansi::CYAN, ansi::RESET);

        // Axis row
        std::printf("%s║%s   %s0%s    └",
                    ansi::CYAN, ansi::RESET, ansi::YELLOW, ansi::RESET);
        std::printf("%s", ansi::GRAY);
        for (int i = 0; i < SPARK_LEN + 1; ++i) std::printf("─");
        char ts[16]; std::snprintf(ts, sizeof(ts), " %ds", SPARK_LEN);
        // Visible so far: 3 + 1 + 4 + 1 + SPARK_LEN + 1 = SPARK_LEN + 10
        // + ts
        int ts_pad = FULL_VW - SPARK_LEN - 10 - static_cast<int>(strlen(ts));
        std::printf("%s%*s%s%s%s %s║%s\n",
                    ansi::RESET, ts_pad, "", ansi::GRAY, ts, ansi::RESET,
                    ansi::CYAN, ansi::RESET);

        hline("╠", "═", "╣");
    }

    void section_risk_latency() noexcept {
        const uint64_t acc   = risk_.orders_accepted();
        const uint64_t rej   = risk_.orders_rejected();
        const uint64_t total = acc + rej > 0 ? acc + rej : 1;
        const double acc_pct = static_cast<double>(acc) / total * 100.0;
        const double rej_pct = 100.0 - acc_pct;

        char acc_str[32], rej_str[32];
        fmt_comma(acc_str, sizeof(acc_str), acc);
        fmt_comma(rej_str, sizeof(rej_str), rej);

        const uint64_t lat_min  = tracker_.min_ns();
        const uint64_t lat_p50  = tracker_.percentile(0.50);
        const uint64_t lat_p99  = tracker_.percentile(0.99);
        const uint64_t lat_max  = tracker_.max_ns();
        const uint64_t lat_ref  = lat_max > 0 ? lat_max : 1;

        char min_s[24], p50_s[24], p99_s[24], max_s[24];
        fmt_lat(min_s, sizeof(min_s), lat_min);
        fmt_lat(p50_s, sizeof(p50_s), lat_p50);
        fmt_lat(p99_s, sizeof(p99_s), lat_p99);
        fmt_lat(max_s, sizeof(max_s), lat_max);

        static constexpr int BAR_W = 12;

        char left[256], right[256];

        // Row 0: headings
        std::snprintf(left,  sizeof(left),  "%sRISK ENGINE%s", ansi::BOLD, ansi::RESET);
        std::snprintf(right, sizeof(right), "%sEND-TO-END LATENCY%s", ansi::BOLD, ansi::RESET);
        split_row(left, right);

        // Row 1: accepted | min
        std::snprintf(left,  sizeof(left),
                      "  %s✓%s Accepted  %s%-12s%s %5.1f%%",
                      ansi::BGREEN, ansi::RESET,
                      ansi::BGREEN, acc_str, ansi::RESET, acc_pct);
        std::snprintf(right, sizeof(right),
                      "  min : %s%s%s",
                      ansi::GRAY, min_s, ansi::RESET);
        split_row(left, right);

        // Row 2: rejected | p50 + bar
        std::snprintf(left, sizeof(left),
                      "  %s✗%s Rejected  %s%-12s%s %5.1f%%",
                      ansi::BRED, ansi::RESET,
                      ansi::BRED, rej_str, ansi::RESET, rej_pct);
        // Right column has inline bar — write manually
        std::printf("%s║%s ", ansi::CYAN, ansi::RESET);
        vcell(left, L_COL);
        std::printf(" %s│%s  p50 : %s%s%s ", ansi::CYAN, ansi::RESET,
                    ansi::GREEN, p50_s, ansi::RESET);
        draw_bar(static_cast<double>(lat_p50) / lat_ref, BAR_W, ansi::GREEN);
        int pad2 = R_COL - 8 - 10 - BAR_W;
        if (pad2 < 0) pad2 = 0;
        std::printf("%*s %s║%s\n", pad2, "", ansi::CYAN, ansi::RESET);

        // Row 3: (blank) | p99 + bar
        std::printf("%s║%s ", ansi::CYAN, ansi::RESET);
        vcell("", L_COL);
        std::printf(" %s│%s  p99 : %s%s%s ", ansi::CYAN, ansi::RESET,
                    ansi::BYELLOW, p99_s, ansi::RESET);
        draw_bar(static_cast<double>(lat_p99) / lat_ref, BAR_W, ansi::BYELLOW);
        int pad3 = R_COL - 8 - 10 - BAR_W;
        if (pad3 < 0) pad3 = 0;
        std::printf("%*s %s║%s\n", pad3, "", ansi::CYAN, ansi::RESET);

        // Row 4: (blank) | max
        std::snprintf(right, sizeof(right),
                      "  max : %s%s%s",
                      ansi::BRED, max_s, ansi::RESET);
        split_row("", right);

        hline_split();
    }

    void section_recent_orders() noexcept {
        {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "%sRECENT ORDERS%s", ansi::BOLD, ansi::RESET);
            full_row(buf);
        }

        OrderSnapshot snaps[8];
        const int cnt = recent_.read_recent(snaps, 8);

        for (int i = 0; i < 8; ++i) {
            if (i < cnt && snaps[i].valid) {
                const auto& s = snaps[i];
                const bool  ok       = (s.status == OrderStatus::Accepted);
                const char* side_clr = (s.side == Side::Buy) ? ansi::BGREEN : ansi::BRED;
                const char* side_str = (s.side == Side::Buy) ? "BUY " : "SELL";
                const char* stat_clr = ok ? ansi::BGREEN : ansi::BRED;
                const char* stat_str = ok ? "✓ ACCEPTED" : "✗ REJECTED";
                char lat_s[24]; fmt_lat(lat_s, sizeof(lat_s), s.latency_ns);

                char row[512];
                std::snprintf(row, sizeof(row),
                    "  %s#%-9llu%s  %s%-4s%s  %s%s%-4s%s  %s%8.2f%s  qty=%s%4.0f%s  %s%s%s  %s%s%s",
                    ansi::GRAY,   static_cast<unsigned long long>(s.order_id), ansi::RESET,
                    ansi::BCYAN,  s.symbol, ansi::RESET,
                    side_clr, ansi::BOLD, side_str, ansi::RESET,
                    ansi::BWHITE, s.price, ansi::RESET,
                    ansi::YELLOW, s.quantity, ansi::RESET,
                    stat_clr, stat_str, ansi::RESET,
                    ansi::GRAY,   lat_s, ansi::RESET);
                full_row(row);
            } else {
                full_row("");
            }
        }

        hline("╠", "═", "╣");
    }

    void section_footer() noexcept {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "  Pool cap: %zu   In-use: %s%llu%s   Free: %s%llu%s"
                      "              %sPress Ctrl+C to stop%s",
                      PoolSize,
                      ansi::YELLOW, static_cast<unsigned long long>(pool_.in_use()),   ansi::RESET,
                      ansi::BGREEN, static_cast<unsigned long long>(pool_.available()), ansi::RESET,
                      ansi::GRAY, ansi::RESET);
        full_row(buf);
        hline("╚", "═", "╝");
    }

public:
    Dashboard(std::atomic<uint64_t>&                  processed,
              LockFreeRingBuffer<Order, RingCapacity>& ring,
              RiskEngine&                              risk,
              LatencyTracker&                          tracker,
              RecentOrdersBuffer&                      recent,
              MemoryPool<Order, PoolSize>&             pool,
              std::atomic<bool>&                       running) noexcept
        : processed_(processed), ring_(ring), risk_(risk),
          tracker_(tracker), recent_(recent), pool_(pool), running_(running)
    {
        spark_.fill(0.0);
        start_ns_       = now_ns();
        last_sample_ns_ = start_ns_;
    }

    // Call once per second to capture a throughput sample for the sparkline
    void sample_throughput() noexcept {
        const uint64_t now  = now_ns();
        const uint64_t cur  = processed_.load(std::memory_order_relaxed);
        const double   dt_s = static_cast<double>(now - last_sample_ns_) / 1e9;
        if (dt_s < 0.1) return;

        cur_rate_  = static_cast<double>(cur - last_proc_) / dt_s;
        peak_rate_ = std::max(peak_rate_, cur_rate_);

        spark_[spark_head_] = cur_rate_;
        spark_head_ = (spark_head_ + 1) % SPARK_LEN;

        last_proc_       = cur;
        last_sample_ns_  = now;
    }

    // Full redraw — reposition cursor to top-left then repaint all sections
    void render() noexcept {
        std::printf("%s", ansi::HOME); // move to (1,1) without clearing = no flicker
        section_header();
        section_status_ring();
        section_sparkline();
        section_risk_latency();
        section_recent_orders();
        section_footer();
        std::fflush(stdout);
    }

    static void init_terminal() noexcept {
        std::printf("%s%s%s", ansi::HIDE_CURSOR, ansi::CLEAR, ansi::HOME);
        std::fflush(stdout);
    }

    static void restore_terminal() noexcept {
        std::printf("\n%s", ansi::SHOW_CURSOR);
        std::fflush(stdout);
    }
};
