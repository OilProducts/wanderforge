#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace wf {

class StreamingService {
public:
    using Task = std::function<void()>;

    void start(std::size_t thread_count);
    void stop();

    void submit(Task task);
    bool busy() const;
    bool idle() const;
    std::size_t pending_tasks() const;

private:
    void worker_main(std::size_t index);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> tasks_;
    std::vector<std::thread> workers_;
    std::atomic<bool> quit_{false};
    std::atomic<std::size_t> active_workers_{0};
};

} // namespace wf
