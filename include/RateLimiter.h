#pragma once
#include <string>
#include <unordered_map>
#include <chrono>
#include <mutex>
#include "Metrics.h"

class RateLimiter {
private:
    struct TokenBucket {
        double tokens;
        std::chrono::steady_clock::time_point last_updated;
    };
    std::unordered_map<uint32_t, TokenBucket> buckets;
    double capacity = 50.0;
    double fill_rate = 50.0; // 50 req/sec
    std::mutex rl_mutex;

public:
    static RateLimiter& getInstance() {
        static RateLimiter instance;
        return instance;
    }

    bool allow(uint32_t ip) {
        std::lock_guard<std::mutex> lock(rl_mutex);
        auto now = std::chrono::steady_clock::now();
        if (buckets.find(ip) == buckets.end()) {
            buckets[ip] = {capacity - 1.0, now};
            return true;
        }

        auto& bucket = buckets[ip];
        double elapsed_sec = std::chrono::duration<double>(now - bucket.last_updated).count();
        bucket.tokens += elapsed_sec * fill_rate;
        if (bucket.tokens > capacity) bucket.tokens = capacity;

        bucket.last_updated = now;

        if (bucket.tokens >= 1.0) {
            bucket.tokens -= 1.0;
            return true;
        }
        
        Metrics::getInstance().blocked_requests++;
        return false;
    }
};
