#include "streaming_service.h"

#include <algorithm>

namespace wf {

namespace {
std::size_t resolve_thread_count(std::size_t count_hint) {
    if (count_hint == 0) {
        unsigned int hw = std::thread::hardware_concurrency();
        return static_cast<std::size_t>(hw == 0 ? 1u : hw);
    }
    return count_hint;
}
} // namespace

void StreamingService::start(std::size_t thread_count) {
    stop();
    quit_.store(false, std::memory_order_relaxed);
    workers_.clear();
    std::size_t resolved = resolve_thread_count(thread_count);
    workers_.reserve(resolved);
    for (std::size_t i = 0; i < resolved; ++i) {
        workers_.emplace_back([this, i]() { worker_main(i); });
    }
}

void StreamingService::stop() {
    quit_.store(true, std::memory_order_relaxed);
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.clear();
    }
    active_workers_.store(0, std::memory_order_relaxed);
}

void StreamingService::submit(Task task, bool drop_pending) {
    if (!task) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (drop_pending) {
            tasks_.clear();
        }
        tasks_.push_back(std::move(task));
    }
    cv_.notify_one();
}

bool StreamingService::busy() const {
    return active_workers_.load(std::memory_order_relaxed) > 0;
}

bool StreamingService::idle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.empty() && active_workers_.load(std::memory_order_relaxed) == 0;
}

std::size_t StreamingService::pending_tasks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

void StreamingService::worker_main(std::size_t /*index*/) {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] {
                return quit_.load(std::memory_order_relaxed) || !tasks_.empty();
            });
            if (quit_.load(std::memory_order_relaxed) && tasks_.empty()) {
                break;
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
            active_workers_.fetch_add(1, std::memory_order_relaxed);
        }

        if (task) {
            task();
        }

        active_workers_.fetch_sub(1, std::memory_order_relaxed);
    }
}

} // namespace wf

