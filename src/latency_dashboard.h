#pragma once
// =============================================================================
// latency_dashboard.h -- A dashboard focused on Latency
// =============================================================================

#include "dashboard.h"

// A dashboard that provides a detailed view of the system's latency.
class LatencyDashboard : public Dashboard {
public:
    LatencyDashboard(std::atomic<uint64_t>& processed, 
                     LockFreeRingBuffer<Order, RING_CAPACITY>& ring, 
                     RiskEngine& risk, 
                     LatencyTracker& tracker, 
                     RecentOrdersBuffer& recent, 
                     MemoryPool<Order, POOL_SIZE>& pool, 
                     std::atomic<bool>& running)
        : Dashboard(processed, ring, risk, tracker, recent, pool, running) {}

    void render() override {
        // Implementation of the latency-focused dashboard will go here
    }
};
