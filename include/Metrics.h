#pragma once
#include <atomic>

struct Metrics {
    std::atomic<uint64_t> total_requests{0};
    std::atomic<uint64_t> active_connections{0};
    std::atomic<uint64_t> bytes_transferred{0};

    static Metrics& getInstance() {
        static Metrics instance;
        return instance;
    }
};
