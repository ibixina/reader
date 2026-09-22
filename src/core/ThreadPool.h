#pragma once
#include "core/CancellationToken.h"
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

namespace reader {

// Fixed worker pool backing render / extraction / analysis / search /
// network lanes (§7). One pool per lane keeps AI off the render path.
class ThreadPool {
public:
    explicit ThreadPool(std::size_t threads = std::max<std::size_t>(2, std::thread::hardware_concurrency() / 2)) {
        workers_.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i)
            workers_.emplace_back([this] { run(); });
    }
    ~ThreadPool() {
        shutdown();
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_ && workers_.empty()) return;
            stop_ = true;
            while (!queue_.empty()) queue_.pop();
        }
        cv_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
        workers_.clear();
    }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template <typename F>
    std::future<void> submit(F&& f, CancellationToken token = {}) {
        auto task = std::make_shared<std::packaged_task<void()>>(
            [fn = std::forward<F>(f), token]() mutable { if (!token.cancelled()) fn(); });
        std::future<void> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) throw std::runtime_error("ThreadPool is shut down");
            queue_.push([task] { (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

    std::size_t pending() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    void run() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                job = std::move(queue_.front());
                queue_.pop();
            }
            job();
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
    bool stop_ = false;
};

} // namespace reader
