#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace nim {

// A persistent pool of worker threads for short per-frame fan-outs (material constants, culling).
// Starting threads per frame costs more than the work itself; these wait on a condition variable.
// parallel_for runs fn(i) for i in [0, n) on the workers and the calling thread, and returns when
// all are done. Calls must not nest and come from one thread at a time.
class WorkerPool {
public:
    static WorkerPool& instance() {
        static WorkerPool pool;
        return pool;
    }
    size_t threads() const { return workers_.size() + 1; }

    template <class Fn>
    void parallel_for(size_t n, Fn&& fn, size_t min_per_thread = 1) {
        if (n == 0) return;
        const size_t lanes = std::min(threads(), (n + min_per_thread - 1) / std::max<size_t>(min_per_thread, 1));
        if (lanes <= 1) {
            for (size_t i = 0; i < n; ++i) fn(i);
            return;
        }
        std::function<void(size_t)> job = [&fn](size_t i) { fn(i); };
        {
            std::lock_guard lock(mutex_);
            job_ = &job;
            count_ = n;
            next_.store(0, std::memory_order_relaxed);
            wanted_ = lanes - 1;
            claimed_ = finished_ = 0;
            ++generation_;
        }
        wake_.notify_all();
        run();
        std::unique_lock lock(mutex_);
        done_.wait(lock, [&] { return finished_ == wanted_; });
        job_ = nullptr;
        wanted_ = 0;
    }

    ~WorkerPool() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        for (auto& t : workers_) t.join();
    }

private:
    WorkerPool() {
        const unsigned n = std::max(1u, std::min(std::thread::hardware_concurrency(), 16u));
        for (unsigned i = 1; i < n; ++i) workers_.emplace_back([this] { loop(); });
    }
    void run() {
        for (size_t i; (i = next_.fetch_add(1, std::memory_order_relaxed)) < count_;) (*job_)(i);
    }
    void loop() {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                // Workers beyond the lanes a call asked for stay asleep.
                wake_.wait(lock, [&] { return stop_ || (generation_ != seen && claimed_ < wanted_); });
                if (stop_) return;
                seen = generation_;
                ++claimed_;
            }
            run();
            {
                std::lock_guard lock(mutex_);
                if (++finished_ == wanted_) done_.notify_one();
            }
        }
    }

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable wake_, done_;
    std::function<void(size_t)>* job_ = nullptr;
    std::atomic<size_t> next_{0};
    size_t count_ = 0, wanted_ = 0, claimed_ = 0, finished_ = 0;
    uint64_t generation_ = 0;
    bool stop_ = false;
};

} // namespace nim
