#pragma once
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>

class WorkerPool {
public:
    WorkerPool(size_t threads);
    ~WorkerPool();
    void enqueue(std::function<void()> task);
    
    size_t queue_size() {
        std::lock_guard<std::mutex> lock(queue_mutex);
        return tasks.size();
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
};
