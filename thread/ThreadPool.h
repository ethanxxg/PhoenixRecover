#pragma once

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <functional>
#include <future>
#include <atomic>
#include <stdexcept>

// ---------------------------------------------------------------------------
// ThreadPool – simple fixed-size thread pool.
//   Submit(f, args...) returns a std::future<result_type>.
// ---------------------------------------------------------------------------
class ThreadPool
{
public:
    explicit ThreadPool(
        size_t threadCount = std::thread::hardware_concurrency())
    {
        if (threadCount == 0) threadCount = 1;
        m_workers.reserve(threadCount);
        for (size_t i = 0; i < threadCount; ++i) {
            m_workers.emplace_back([this] { WorkerLoop(); });
        }
    }

    ~ThreadPool() { Shutdown(); }

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Submit a callable and return a future for its result.
    template<typename Func, typename... Args>
    auto Submit(Func&& func, Args&&... args)
        -> std::future<std::invoke_result_t<Func, Args...>>
    {
        using Ret = std::invoke_result_t<Func, Args...>;

        if (m_stop.load())
            throw std::runtime_error("ThreadPool is stopped");

        auto task = std::make_shared<std::packaged_task<Ret()>>(
            std::bind(std::forward<Func>(func), std::forward<Args>(args)...));

        std::future<Ret> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_tasks.emplace([task]() { (*task)(); });
        }
        m_cv.notify_one();
        return fut;
    }

    void Shutdown()
    {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_stop.store(true);
        }
        m_cv.notify_all();
        for (auto& w : m_workers)
            if (w.joinable()) w.join();
        m_workers.clear();
    }

    size_t GetThreadCount()      const { return m_workers.size(); }
    size_t GetPendingTaskCount() const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_tasks.size();
    }

private:
    void WorkerLoop()
    {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(m_mutex);
                m_cv.wait(lk, [this] {
                    return m_stop.load() || !m_tasks.empty();
                });
                if (m_stop.load() && m_tasks.empty()) return;
                task = std::move(m_tasks.front());
                m_tasks.pop();
            }
            task();
        }
    }

    std::vector<std::thread>          m_workers;
    std::queue<std::function<void()>> m_tasks;
    mutable std::mutex                m_mutex;
    std::condition_variable           m_cv;
    std::atomic<bool>                 m_stop{false};
};
