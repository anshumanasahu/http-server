#pragma once
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <fstream>
#include <sstream>

struct Metrics {
    std::atomic<uint64_t> total_requests{0};
    std::atomic<uint64_t> active_connections{0};
    std::atomic<uint64_t> bytes_transferred{0};
    std::atomic<uint64_t> blocked_requests{0};
    std::atomic<uint64_t> keep_alive_hits{0};
    std::atomic<uint64_t> new_connections{0};

    // Multi-protocol counters
    std::atomic<long long> http_requests{0};
    std::atomic<long long> ftp_commands{0};
    std::atomic<long long> smtp_emails{0};
    std::atomic<long long> imap_commands{0};

    std::deque<std::string> recent_logs;
    std::mutex logs_mutex;

    void addLog(const std::string& log_entry) {
        std::lock_guard<std::mutex> lock(logs_mutex);
        recent_logs.push_back(log_entry);
        if (recent_logs.size() > 100) {
            recent_logs.pop_front();
        }
    }
    
    std::string flushLogs() {
        std::lock_guard<std::mutex> lock(logs_mutex);
        std::string res = "[";
        for (size_t i = 0; i < recent_logs.size(); ++i) {
            res += recent_logs[i];
            if (i < recent_logs.size() - 1) res += ",";
        }
        res += "]";
        recent_logs.clear();
        return res;
    }
    
    std::string read_tcp_states() {
        std::ifstream file("/proc/net/tcp");
        if (!file.is_open()) return "{\"ESTABLISHED\":0,\"TIME_WAIT\":0,\"CLOSE_WAIT\":0}";
        std::string line;
        int established = 0, time_wait = 0, close_wait = 0;
        std::getline(file, line);
        while (std::getline(file, line)) {
            std::istringstream iss(line);
            std::string token;
            for (int i = 0; i < 4; ++i) iss >> token;
            if (token == "01") established++;
            else if (token == "06") time_wait++;
            else if (token == "08") close_wait++;
        }
        return "{\"ESTABLISHED\":" + std::to_string(established) + 
               ",\"TIME_WAIT\":" + std::to_string(time_wait) + 
               ",\"CLOSE_WAIT\":" + std::to_string(close_wait) + "}";
    }

    static Metrics& getInstance() {
        static Metrics instance;
        return instance;
    }
};
