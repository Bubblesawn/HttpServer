/**
 * @file thread_pool.cc
 * @brief 线程池类的实现文件
 * 
 * 实现ThreadPool类中声明的所有非模板方法。
 * 包含线程池的创建、任务执行、优雅关闭等功能。
 */

#include "thread_pool.h"
#include <iostream>

/**
 * @brief 构造函数
 * 
 * 创建线程池并启动指定数量的工作线程。
 * 每个工作线程都会运行workerThread()函数。
 * 
 * @param numThreads 工作线程数量
 */
ThreadPool::ThreadPool(size_t numThreads)
    : m_stop(false),       // 初始状态为不停止
      m_activeTasks(0) {    // 初始活跃任务数为0
    
    // 预先分配线程容量的空间，避免动态扩容开销
    m_workers.reserve(numThreads);
    
    // 创建并启动工作线程
    for (size_t i = 0; i < numThreads; ++i) {
        // 使用emplace_back避免不必要的复制
        // 每个线程都运行workerThread成员函数
        m_workers.emplace_back(&ThreadPool::workerThread, this);
    }
}

/**
 * @brief 析构函数
 * 
 * 销毁线程池，调用shutdown()确保资源正确释放。
 */
ThreadPool::~ThreadPool() {
    shutdown();  // 确保线程池正确关闭
}

/**
 * @brief 关闭线程池
 * 
 * 执行优雅关闭：
 * 1. 设置停止标志，阻止新任务加入
 * 2. 唤醒所有等待中的工作线程
 * 3. 等待所有工作线程完成并退出
 * 
 * @note 调用此方法后，所有工作线程将退出
 */
void ThreadPool::shutdown() {
    {
        // 获取互斥锁
        std::unique_lock<std::mutex> lock(m_queueMutex);
        
        // 设置停止标志
        // 这将导致所有等待中的workerThread()退出
        m_stop.store(true);
    }
    
    // 唤醒所有等待中的工作线程
    // 这样它们可以检查停止标志并退出
    m_condition.notify_all();

    // 等待所有工作线程结束
    // 使用joinable()检查线程是否可合并，然后join等待
    for (auto& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

/**
 * @brief 工作线程函数
 * 
 * 每个工作线程运行的函数，实现消费者逻辑：
 * 1. 获取互斥锁
 * 2. 等待条件变量（直到有新任务或需要停止）
 * 3. 从队列取出任务
 * 4. 释放互斥锁
 * 5. 执行任务（捕获异常）
 * 6. 重复步骤1
 * 
 * 使用条件变量的wait()实现高效等待，避免忙轮询。
 */
void ThreadPool::workerThread() {
    // 持续运行直到收到停止信号
    while (true) {
        std::function<void()> task;

        {
            // 获取互斥锁，保护任务队列
            std::unique_lock<std::mutex> lock(m_queueMutex);

            /**
             * @brief 条件变量等待
             * 
             * wait()会释放锁并阻塞当前线程，直到满足以下任一条件：
             * 1. m_stop.load()为true（线程池需要停止）
             * 2. m_tasks不为空（有新的任务需要处理）
             * 
             * 这种方式比忙轮询（busy spin）高效得多，
             * 因为线程在等待时不会消耗CPU周期。
             */
            m_condition.wait(lock, [this] {
                return m_stop.load() || !m_tasks.empty();
            });

            /**
             * @brief 检查退出条件
             * 
             * 当线程池停止且任务队列为空时，线程可以安全退出
             */
            if (m_stop.load() && m_tasks.empty()) {
                return;  // 线程结束
            }

            // 从队列取出任务
            // 使用std::move避免不必要的复制
            task = std::move(m_tasks.front());
            m_tasks.pop();
        }

        // 任务已取出，释放了互斥锁
        // 现在可以在锁外执行任务，避免阻塞其他线程

        if (task) {
            // 增加活跃任务计数
            ++m_activeTasks;
            
            try {
                // 执行任务
                task();
            } catch (const std::exception& e) {
                // 捕获标准异常并输出错误信息
                std::cerr << "Task exception: " << e.what() << std::endl;
            } catch (...) {
                // 捕获所有其他异常
                std::cerr << "Unknown task exception" << std::endl;
            }
            
            // 任务完成，减少活跃任务计数
            --m_activeTasks;
        }
    }
}

/**
 * @brief 获取任务队列大小
 * 
 * 获取当前等待执行的任务数量。
 * 
 * @return size_t 队列中的任务数量
 * 
 * @note 需要获取锁才能安全访问队列
 */
size_t ThreadPool::getQueueSize() {
    std::unique_lock<std::mutex> lock(m_queueMutex);
    return m_tasks.size();
}

/**
 * @brief 获取线程数量
 * 
 * @return size_t 工作线程的总数
 */
size_t ThreadPool::getThreadCount() const {
    return m_workers.size();
}
