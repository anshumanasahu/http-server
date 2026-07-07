#include "ThreadPool.h"
#include <iostream>

ThreadPool::ThreadPool(size_t num_threads, size_t max_queue_size, std::function<void(int)> handler)
    : queue_(max_queue_size), handler_(std::move(handler)) {
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back(&ThreadPool::worker_loop, this);
    }
}

ThreadPool::~ThreadPool() {
    // Push poison pills (-1) to wake up all workers and tell them to exit
    for (size_t i = 0; i < workers_.size(); ++i) {
        // We use a retry loop because try_push might fail if queue is full.
        // Wait, if queue is full, popping will make space. So we can just poll or block push.
        // Since try_push doesn't block, we could loop until it succeeds.
        // A better approach in a real concurrent queue is to have a block_push,
        // but for now we can just yield if queue is full during shutdown.
        while (!queue_.try_push(-1)) {
            std::this_thread::yield();
        }
    }

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

bool ThreadPool::enqueue(int client_socket) {
    return queue_.try_push(client_socket);
}

void ThreadPool::worker_loop() {
    while (true) {
        int client_socket = queue_.pop();
        
        // Poison pill check
        if (client_socket == -1) {
            break;
        }

        try {
            handler_(client_socket);
        } catch (const std::exception& e) {
            std::cerr << "Exception in worker thread: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "Unknown exception in worker thread." << std::endl;
        }
    }
}
