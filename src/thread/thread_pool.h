/**
 * @file thread_pool.h
 * @brief 线程池类的头文件定义
 * 
 * 本文件定义了ThreadPool类，提供一个高效的线程池实现。
 * 用于管理多个工作线程并处理任务队列。
 */

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

// C++标准库头文件
#include <thread>         // 线程支持
#include <mutex>          // 互斥锁
#include <condition_variable>  // 条件变量
#include <queue>          // 队列容器
#include <functional>     // 函数对象
#include <vector>         // 向量容器
#include <atomic>         // 原子操作

/**
 * @brief 线程池类
 * 
 * 一个高效的工作线程池实现，采用生产者-消费者模式。
 * 主要特点：
 * - 固定数量的工作线程
 * - 线程安全的任务队列
 * - 条件变量实现的阻塞等待
 * - 支持任务异常捕获
 * - 优雅关闭机制
 * 
 * 工作原理：
 * 1. 构造函数创建指定数量的工作线程
 * 2. 主线程通过enqueue()方法向任务队列添加任务
 * 3. 工作线程从任务队列取出任务并执行
 * 4. 使用条件变量实现任务的异步等待
 * 
 * @note 这是C++11实现的线程池，利用了现代C++的并发特性
 */
class ThreadPool {
public:
    /**
     * @brief 构造函数
     * 
     * 创建线程池并启动指定数量的工作线程。
     * 
     * @param numThreads 工作线程的数量
     * 
     * @par 示例：
     * @code
     * ThreadPool pool(4);  // 创建4个工作线程
     * @endcode
     */
    explicit ThreadPool(size_t numThreads);

    /**
     * @brief 析构函数
     * 
     * 销毁线程池，确保所有工作线程正确退出。
     * 会调用shutdown()等待所有任务完成。
     */
    ~ThreadPool();

    /**
     * @brief 向任务队列添加任务
     * 
     * 将一个可调用对象（函数、lambda、bind等）添加到任务队列。
     * 线程池会自动选择一个空闲的工作线程来执行任务。
     * 
     * @tparam F 可调用对象的类型
     * @param task 要执行的任务（可调用对象）
     * 
     * @par 示例：
     * @code
     * pool.enqueue([]() {
     *     std::cout << "Task executed" << std::endl;
     * });
     * 
     * pool.enqueue([](int a, int b) {
     *     return a + b;
     * }, 1, 2);
     * @endcode
     */
    template<typename F>
    void enqueue(F&& task);
    
    /**
     * @brief 关闭线程池
     * 
     * 优雅关闭线程池：
     * 1. 设置停止标志
     * 2. 唤醒所有等待中的工作线程
     * 3. 等待所有工作线程结束
     * 
     * @note 调用shutdown()后不应再添加新任务
     */
    void shutdown();

    /**
     * @brief 获取任务队列大小
     * 
     * @return size_t 当前等待执行的任务数量
     */
    size_t getQueueSize();

    /**
     * @brief 获取线程数量
     * 
     * @return size_t 工作线程的总数
     */
    size_t getThreadCount() const;

private:
    /**
     * @brief 工作线程函数
     * 
     * 每个工作线程运行的函数。
     * 持续从任务队列取出任务并执行，直到收到停止信号。
     */
    void workerThread();

    //================== 成员变量 ==================

    /** 工作线程集合 */
    std::vector<std::thread> m_workers;

    /** 任务队列（存储待执行的可调用对象） */
    std::queue<std::function<void()>> m_tasks;

    /** 任务队列的互斥锁（保证线程安全） */
    std::mutex m_queueMutex;

    /** 条件变量（用于任务到达通知和线程唤醒） */
    std::condition_variable m_condition;

    /** 停止标志（原子操作保证线程安全） */
    std::atomic<bool> m_stop;

    /** 当前正在执行的任务数量 */
    std::atomic<size_t> m_activeTasks;
};

/**
 * @brief 向任务队列添加任务（模板实现）
 * 
 * 模板实现允许接受任意可调用对象及其参数。
 * 
 * @tparam F 可调用对象类型
 * @tparam Args 参数类型
 * @param task 可调用对象
 * @param args 传递给可调用对象的参数
 */
template<typename F>
void ThreadPool::enqueue(F&& task) {
    {
        // 获取互斥锁，保证队列操作的线程安全
        std::unique_lock<std::mutex> lock(m_queueMutex);

        // 如果线程池已停止，不再接受新任务
        if (m_stop.load()) {
            return;
        }

        // 将任务包装成void()并加入队列
        // 使用std::forward保持参数的值类别（左值/右值）
        m_tasks.emplace(std::forward<F>(task));
    }
    // 释放锁后通知一个工作线程有新任务
    m_condition.notify_one();
}

#endif // THREAD_POOL_H
