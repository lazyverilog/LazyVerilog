#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

/// Runs read-only request handlers off the thread that reads messages.
///
/// clangd keeps message *dispatch* on one thread -- notifications stay in the
/// order they arrived, and a document update is applied before anything queued
/// behind it -- and hands the *work* of a request to other threads, replying
/// from whichever one finishes.  This is that pool.
///
/// Small on purpose.  Its job is to stop one whole-file request standing in
/// front of the completion the user is waiting for, not to run many at once;
/// the cores beyond that belong to background indexing.
class RequestPool {
public:
    explicit RequestPool(size_t threads) {
        workers_.reserve(threads);
        for (size_t i = 0; i < threads; ++i)
            workers_.emplace_back([this] { run(); });
    }

    ~RequestPool() { shutdown(); }

    RequestPool(const RequestPool&) = delete;
    RequestPool& operator=(const RequestPool&) = delete;

    /// Queue @p task.  False when the pool is shutting down and took nothing --
    /// the caller still owes the client a reply and must run it itself.
    bool submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_)
                return false;
            queue_.push_back(std::move(task));
        }
        cv_.notify_one();
        return true;
    }

    /// Stop accepting work, answer everything already accepted, then join the
    /// workers.  Idempotent.
    ///
    /// It drains rather than abandons, because every task here is a reply a
    /// client is waiting for.  Waiting is safe: the methods deferred to this
    /// pool are pure computations over an immutable snapshot, so they finish on
    /// their own and wait on nothing the caller still owes them.
    ///
    /// Closing the door first is what bounds that wait.  Draining before
    /// setting `stop_` left submit() accepting the whole time, so a client still
    /// sending requests could keep the queue non-empty and this call would never
    /// return.  The normal exit path stops the transport before reaching here,
    /// but the teardown this also covers -- a test harness, or stdin closing
    /// under a client that never sends `exit` -- does not.  Nothing is dropped:
    /// submit() answers false once stopped, and its caller runs the task itself.
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        // Before waiting, not after: a worker parked on an empty queue has to be
        // woken to see `stop_`, and a worker parked while the queue is *not*
        // empty -- possible when an earlier notify_one() was consumed by a
        // worker that then finished -- has to be woken to drain it.
        cv_.notify_all();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            idle_cv_.wait(lock, [this] { return queue_.empty() && running_ == 0; });
        }
        for (auto& worker : workers_)
            if (worker.joinable())
                worker.join();
        workers_.clear();
    }

private:
    void run() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (queue_.empty())
                    return; // stop_, and nothing left to answer
                task = std::move(queue_.front());
                queue_.pop_front();
                ++running_;
            }
            // A handler that throws must not take the pool down with it: the
            // request is answered or not, but the next one still runs.
            try {
                task();
            } catch (const std::exception& e) {
                std::cerr << "[lazyverilog] request worker error: " << e.what() << "\n";
            } catch (...) {
                std::cerr << "[lazyverilog] request worker error\n";
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                --running_;
            }
            idle_cv_.notify_all();
        }
    }

    std::mutex                        mutex_;
    std::condition_variable           cv_;
    /// Signalled whenever the pool might have gone idle.  See shutdown().
    std::condition_variable           idle_cv_;
    std::deque<std::function<void()>> queue_;
    size_t                            running_{0};
    bool                              stop_{false};
    std::vector<std::thread>          workers_;
};
