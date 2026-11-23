#include "parallel.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

struct Job
{
    const std::function<void(int)>* task = nullptr;
    int count = 0;
    std::atomic<int> next{ 0 };
    std::atomic<int> done{ 0 };
    std::mutex m;
    std::condition_variable finished;

    // claim and run one index; false when nothing is left to claim
    bool runOne()
    {
        const int i = next.fetch_add(1);
        if (i >= count)
            return false;
        (*task)(i);
        if (done.fetch_add(1) + 1 == count) {
            std::lock_guard<std::mutex> lock(m);
            finished.notify_all();
        }
        return true;
    }
};

class Pool
{
public:
    static Pool& instance()
    {
        // intentionally never destroyed: joining threads from static destructors can deadlock
        // when a plugin DLL/bundle is unloaded (Windows loader lock); the OS reclaims at exit.
        static Pool* pool = new Pool();
        return *pool;
    }

    int concurrency() const { return int(threads_.size()) + 1; }

    void run(int count, const std::function<void(int)>& task)
    {
        if (count <= 0)
            return;
        if (threads_.empty() || count == 1) {
            for (int i = 0; i < count; ++i)
                task(i);
            return;
        }
        auto job = std::make_shared<Job>();
        job->task = &task;
        job->count = count;
        {
            std::lock_guard<std::mutex> lock(m_);
            jobs_.push_back(job);
        }
        wake_.notify_all();

        while (job->runOne()) {}   // the caller helps

        {
            std::unique_lock<std::mutex> lock(job->m);
            job->finished.wait(lock, [&] { return job->done.load() == job->count; });
        }
        std::lock_guard<std::mutex> lock(m_);
        jobs_.erase(std::remove(jobs_.begin(), jobs_.end(), job), jobs_.end());
    }

private:
    Pool()
    {
        const unsigned n = std::max(1u, std::thread::hardware_concurrency());
        for (unsigned i = 1; i < n; ++i)
            threads_.emplace_back([this] { loop(); });
    }

    void loop()
    {
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lock(m_);
                wake_.wait(lock, [&] {
                    // drop jobs whose indices are all claimed
                    while (!jobs_.empty() && jobs_.front()->next.load() >= jobs_.front()->count)
                        jobs_.pop_front();
                    return !jobs_.empty();
                });
                job = jobs_.front();
            }
            while (job->runOne()) {}
        }
    }

    std::mutex m_;
    std::condition_variable wake_;
    std::deque<std::shared_ptr<Job>> jobs_;
    std::vector<std::thread> threads_;
};

} // namespace

void parallelFor(int count, const std::function<void(int)>& task)
{
    Pool::instance().run(count, task);
}

int parallelConcurrency()
{
    return Pool::instance().concurrency();
}
