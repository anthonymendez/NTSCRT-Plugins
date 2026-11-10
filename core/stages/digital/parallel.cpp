// parallel.cpp — persistent std::thread pool behind digital::parallel_for.
#include "parallel.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace ntscrt::digital {
namespace {

thread_local bool t_inside = false;  // running a chunk: nested calls run inline

struct Job {
    int n = 0;
    int chunk = 1;
    detail::RangeFn fn = nullptr;
    void *ctx = nullptr;
    std::atomic<int> next{0};
    std::atomic<int> remaining{0};  // iterations not yet finished
    int holders = 0;                // workers holding a pointer (pool mutex)
    std::mutex m;
    std::condition_variable cv;

    // Run chunks until none are left. Returns after the last one it took.
    void work() {
        for (;;) {
            int b = next.fetch_add(chunk, std::memory_order_relaxed);
            if (b >= n) return;
            int e = std::min(n, b + chunk);
            bool was = t_inside;
            t_inside = true;
            fn(ctx, b, e);
            t_inside = was;
            if (remaining.fetch_sub(e - b, std::memory_order_acq_rel) == e - b) {
                std::lock_guard<std::mutex> lk(m);
                cv.notify_all();
            }
        }
    }
};

class Pool {
public:
    explicit Pool(int threads) {
        for (int i = 0; i < threads - 1; ++i) workers_.emplace_back([this] { loop(); });
    }
    ~Pool() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto &t : workers_) t.join();
    }
    int size() const { return int(workers_.size()) + 1; }

    void run(Job &job) {
        {
            std::lock_guard<std::mutex> lk(m_);
            jobs_.push_back(&job);
        }
        cv_.notify_all();
        job.work();
        {
            std::unique_lock<std::mutex> lk(job.m);
            job.cv.wait(lk, [&] { return job.remaining.load(std::memory_order_acquire) == 0; });
        }
        // No worker may still hold the job when it goes out of scope.
        std::unique_lock<std::mutex> lk(m_);
        auto it = std::find(jobs_.begin(), jobs_.end(), &job);
        if (it != jobs_.end()) jobs_.erase(it);
        idle_.wait(lk, [&] { return job.holders == 0; });
    }

private:
    void loop() {
        std::unique_lock<std::mutex> lk(m_);
        for (;;) {
            cv_.wait(lk, [&] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            Job *job = jobs_.front();
            job->holders++;
            lk.unlock();
            job->work();
            lk.lock();
            // Exhausted: take it off the queue so others don't spin on it.
            if (!jobs_.empty() && jobs_.front() == job) jobs_.pop_front();
            if (--job->holders == 0) idle_.notify_all();
        }
    }

    std::vector<std::thread> workers_;
    std::deque<Job *> jobs_;
    std::mutex m_;
    std::condition_variable cv_, idle_;
    bool stop_ = false;
};

std::mutex g_pool_m;
std::shared_ptr<Pool> g_pool;
int g_requested = 0;

int resolve(int n) {
    if (n > 0) return n;
    unsigned hw = std::thread::hardware_concurrency();
    return int(std::clamp(hw == 0 ? 4u : hw, 1u, 32u));
}

std::shared_ptr<Pool> pool() {
    std::lock_guard<std::mutex> lk(g_pool_m);
    if (!g_pool) g_pool = std::make_shared<Pool>(resolve(g_requested));
    return g_pool;
}

}  // namespace

void set_thread_count(int n) {
    std::shared_ptr<Pool> old;
    {
        std::lock_guard<std::mutex> lk(g_pool_m);
        g_requested = std::max(0, n);
        old = std::move(g_pool);  // rebuilt lazily; old one dies when unused
    }
}

int thread_count() { return pool()->size(); }

namespace detail {
void parallel_for_impl(int n, RangeFn fn, void *ctx) {
    if (n <= 0) return;
    if (t_inside || n == 1) {
        fn(ctx, 0, n);
        return;
    }
    auto p = pool();
    int threads = p->size();
    if (threads <= 1) {
        fn(ctx, 0, n);
        return;
    }
    Job job;
    job.n = n;
    job.chunk = std::max(1, n / (threads * 4));
    job.fn = fn;
    job.ctx = ctx;
    job.remaining.store(n);
    p->run(job);
}
}  // namespace detail

}  // namespace ntscrt::digital
