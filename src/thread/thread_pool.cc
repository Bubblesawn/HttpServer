#include "thread_pool.h"
#include <iostream>

ThreadPool::ThreadPool(size_t numThreads)
    : m_stop(false), m_activeTasks(0) {
    m_workers.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i) {
        m_workers.emplace_back(&ThreadPool::workerThread, this);
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::shutdown() {
    {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        m_stop.store(true);
    }
    m_condition.notify_all();

    for (auto& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::workerThread() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_condition.wait(lock, [this] {
                return m_stop.load() || !m_tasks.empty();
            });

            if (m_stop.load() && m_tasks.empty()) {
                return;
            }

            task = std::move(m_tasks.front());
            m_tasks.pop();
        }

        if (task) {
            ++m_activeTasks;
            try {
                task();
            } catch (const std::exception& e) {
                std::cerr << "Task exception: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Unknown task exception" << std::endl;
            }
            --m_activeTasks;
        }
    }
}

size_t ThreadPool::getQueueSize() {
    std::unique_lock<std::mutex> lock(m_queueMutex);
    return m_tasks.size();
}

size_t ThreadPool::getThreadCount() const {
    return m_workers.size();
}
