#pragma once
#include <unordered_map>
#include <list>
#include <string>
#include <mutex>
#include <optional>

class LRUCache {
private:
    size_t capacity;
    std::list<std::pair<std::string, std::string>> items;
    std::unordered_map<std::string, decltype(items.begin())> cache_map;
    std::mutex cache_mutex;

public:
    LRUCache(size_t capacity) : capacity(capacity) {}

    std::optional<std::string> get(const std::string& key) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = cache_map.find(key);
        if (it == cache_map.end()) {
            return std::nullopt;
        }
        items.splice(items.begin(), items, it->second);
        return it->second->second;
    }

    void put(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = cache_map.find(key);
        if (it != cache_map.end()) {
            items.splice(items.begin(), items, it->second);
            it->second->second = value;
            return;
        }
        
        items.push_front({key, value});
        cache_map[key] = items.begin();
        
        if (cache_map.size() > capacity) {
            cache_map.erase(items.back().first);
            items.pop_back();
        }
    }
};
