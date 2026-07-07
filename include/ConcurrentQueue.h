#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>

template <typename T>
class ConcurrentQueue {
public:
    explicit ConcurrentQueue(size_t capacity) : capacity_(capacity) {}

    // Attempts to push an item. Returns false if the queue is at capacity.
    bool try_push(const T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_.size() >= capacity_) {
            return false;
        }
        queue_.push(item);
        lock.unlock();
        condvar_.notify_one();
        return true;
    }

    // Attempts to push an item using move semantics. Returns false if at capacity.
    bool try_push(T&& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_.size() >= capacity_) {
            return false;
        }
        queue_.push(std::move(item));
        lock.unlock();
        condvar_.notify_one();
        return true;
    }

    // Blocks until an item is available, then pops it and returns it.
    T pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        condvar_.wait(lock, [this] { return !queue_.empty(); });
        T item = std::move(queue_.front());
        queue_.pop();
        return item;
    }

    // Non-blocking pop. Returns std::nullopt if the queue is empty.
    std::optional<T> try_pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return std::nullopt;
        }
        T item = std::move(queue_.front());
        queue_.pop();
        return item;
    }

    size_t size() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    std::queue<T> queue_;
    size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable condvar_;
};
