#pragma once
// =============================================================================
// risk_dashboard.h -- A dashboard focused on the Risk Engine
// =============================================================================

#include "dashboard.h"

// A dashboard that provides a detailed view of the risk engine's performance.
class RiskDashboard : public Dashboard {
public:
    RiskDashboard(std::atomic<uint64_t>& processed, 
                  LockFreeRingBuffer<Order, RING_CAPACITY>& ring, 
                  RiskEngine& risk, 
                  LatencyTracker& tracker, 
                  RecentOrdersBuffer& recent, 
                  MemoryPool<Order, POOL_SIZE>& pool, 
                  std::atomic<bool>& running)
        : Dashboard(processed, ring, risk, tracker, recent, pool, running) {}

    void render() override {
        // Implementation of the risk-focused dashboard will go here
    }
};
