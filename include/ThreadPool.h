#pragma once

#include <vector>
#include <thread>
#include <functional>
#include "ConcurrentQueue.h"

class ThreadPool {
public:
    // Creates a thread pool with the specified number of workers and maximum queue capacity.
    // The handler function will be invoked for each popped client socket descriptor.
    ThreadPool(size_t num_threads, size_t max_queue_size, std::function<void(int)> handler);
    
    // Destructor cleanly shuts down all workers.
    ~ThreadPool();

    // Prevent copying and assignment
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Enqueues a socket descriptor. Returns false if the queue is full.
    bool enqueue(int client_socket);

private:
    std::vector<std::thread> workers_;
    ConcurrentQueue<int> queue_;
    std::function<void(int)> handler_;

    void worker_loop();
};
