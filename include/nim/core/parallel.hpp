#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <exception>
#include <cstdint>
#include <mutex>
#include <thread>
#include <system_error>
#include <vector>

namespace nim {

// A persistent pool of worker threads for short per-frame fan-outs (material constants, culling).
// Starting threads per frame costs more than the work itself; these wait on a condition variable.
// parallel_for runs fn(i) for i in [0, n) on the workers and the calling thread, and returns when
// all are done. Nested or overlapping calls run locally rather than sharing the job state.
// Exceptions are propagated to the submitting thread after all active workers stop.
class WorkerPool {
public:
    static WorkerPool& instance() {
        static WorkerPool pool;
        return pool;
    }
    size_t threads() const { return workers_.size() + 1; }
    // Background rendering must not borrow the viewport's workers or block their caller.
    static void use_current_thread_only() { serial_caller_ = true; }

    template <class Fn>
    void parallel_for(size_t n, Fn&& fn, size_t min_per_thread = 1) {
        if (n == 0) return;
        const size_t grain = std::max<size_t>(min_per_thread, 1);
        const size_t lanes = std::min(threads(), 1 + (n - 1) / grain);
        if (serial_caller_ || in_job_ || lanes <= 1) {
            for (size_t i = 0; i < n; ++i) fn(i);
            return;
        }
        std::unique_lock submission(submission_mutex_, std::try_to_lock);
        if (!submission.owns_lock()) {
            for (size_t i = 0; i < n; ++i) fn(i);
            return;
        }
        std::function<void(size_t)> job = [&fn](size_t i) { fn(i); };
        {
            std::lock_guard lock(mutex_);
            job_ = &job;
            count_ = n;
            // Batch small uniform work, but keep fine-grained scheduling for uneven jobs.
            chunk_ = std::max<size_t>(1, std::min({grain, size_t(32), n / (lanes * 8)}));
            next_.store(0, std::memory_order_relaxed);
            cancelled_.store(false, std::memory_order_relaxed);
            error_ = nullptr;
            wanted_ = lanes - 1;
            claimed_ = finished_ = 0;
            ++generation_;
        }
        wake_.notify_all();
        run();
        std::unique_lock lock(mutex_);
        // Wait for workers that entered the job, even if it was cancelled by an exception.
        wanted_ = claimed_;
        done_.wait(lock, [&] { return finished_ == wanted_; });
        job_ = nullptr;
        wanted_ = 0;
        const auto error = error_;
        lock.unlock();
        if (error) std::rethrow_exception(error);
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
        workers_.reserve(n - 1);
        try {
            for (unsigned i = 1; i < n; ++i) workers_.emplace_back([this] { loop(); });
        } catch (const std::system_error&) {
            // Keep successfully created workers; a constrained process can still run serially.
        }
    }
    void run() {
        in_job_ = true;
        try {
            while (!cancelled_.load(std::memory_order_relaxed)) {
                const size_t first = next_.fetch_add(chunk_, std::memory_order_relaxed);
                if (first >= count_) break;
                const size_t end = first + std::min(chunk_, count_ - first);
                for (size_t i = first; i < end; ++i) (*job_)(i);
            }
        } catch (...) {
            cancelled_.store(true, std::memory_order_relaxed);
            std::lock_guard lock(mutex_);
            if (!error_) error_ = std::current_exception();
        }
        in_job_ = false;
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
    std::mutex mutex_, submission_mutex_;
    std::condition_variable wake_, done_;
    std::function<void(size_t)>* job_ = nullptr;
    std::atomic<size_t> next_{0};
    std::atomic<bool> cancelled_{false};
    std::exception_ptr error_;
    size_t chunk_ = 1;
    size_t count_ = 0, wanted_ = 0, claimed_ = 0, finished_ = 0;
    uint64_t generation_ = 0;
    bool stop_ = false;
    inline static thread_local bool serial_caller_ = false;
    inline static thread_local bool in_job_ = false;
};

} // namespace nim
