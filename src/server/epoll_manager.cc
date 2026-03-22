/**
 * @file epoll_manager.cc
 * @brief epoll事件管理器实现文件
 *
 * 实现EpollManager类中声明的所有方法，
 * 包括epoll创建、事件注册、事件等待和分发等功能。
 */

#include "epoll_manager.h"
#include <iostream>
#include <unordered_map>

/**
 * @brief 构造函数
 *
 * 初始化epoll管理器成员变量
 */
EpollManager::EpollManager()
    : m_epollFd(-1)
    , m_running(true) {
    // 初始化事件数组为零
    memset(m_events, 0, sizeof(m_events));
}

/**
 * @brief 析构函数
 *
 * 自动关闭epoll实例，释放资源
 */
EpollManager::~EpollManager() {
    closeEpoll();
}

/**
 * @brief 创建epoll实例
 *
 * 调用epoll_create1(EPOLL_CLOEXEC)创建epoll文件描述符。
 * EPOLL_CLOEXEC标志确保在exec()调用时自动关闭fd，防止资源泄漏。
 *
 * @return bool 创建成功返回true，失败返回false
 */
bool EpollManager::create() {
    // 如果已创建，先关闭
    if (m_epollFd >= 0) {
        closeEpoll();
    }

    /**
     * @brief 创建epoll实例
     *
     * epoll_create1是epoll_create的改进版本：
     * - EPOLL_CLOEXEC: 设置close-on-exec标志，防止fd泄漏到子进程
     * - 参数0表示不设置特殊标志
     *
     * 返回值：成功返回非负的epoll文件描述符，失败返回-1
     */
    m_epollFd = epoll_create1(EPOLL_CLOEXEC);
    if (m_epollFd < 0) {
        perror("epoll_create1 failed");
        return false;
    }

    LOG_INFO("Epoll instance created, fd: " + std::to_string(m_epollFd));
    return true;
}

/**
 * @brief 关闭epoll实例
 *
 * 关闭epoll文件描述符，清理所有资源
 */
void EpollManager::closeEpoll() {
    if (m_epollFd >= 0) {
        close(m_epollFd);
        m_epollFd = -1;
    }

    // 清空回调函数映射
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_callbacks.clear();
    }

    m_running.store(false);
    LOG_INFO("Epoll instance closed");
}

/**
 * @brief 设置socket为非阻塞模式
 *
 * 使用fcntl获取当前标志，然后添加O_NONBLOCK标志。
 * 非阻塞模式下，IO操作会立即返回，不会阻塞线程。
 *
 * @param fd 要设置的socket文件描述符
 * @return bool 设置成功返回true，失败返回false
 */
bool EpollManager::setNonBlocking(int fd) {
    /**
     * @brief 获取当前文件状态标志
     *
     * F_GETFL: 获取文件状态标志
     */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        perror("fcntl F_GETFL failed");
        return false;
    }

    /**
     * @brief 设置非阻塞标志
     *
     * F_SETFL: 设置文件状态标志
     * O_NONBLOCK: 非阻塞IO标志
     */
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl F_SETFL failed");
        return false;
    }

    return true;
}

/**
 * @brief 将EpollEventType转换为epoll事件标志
 *
 * 根据事件类型和触发模式生成epoll事件标志位
 *
 * @param type 事件类型枚举
 * @param useEdgeTrigger 是否使用边缘触发(ET)模式
 * @return uint32_t epoll事件标志位组合
 *
 * @note EPOLLET(边缘触发): 只在状态变化时通知一次
 * @note 默认水平触发(LT): 只要条件满足就持续通知
 */
uint32_t EpollManager::convertEventType(EpollEventType type, bool useEdgeTrigger) {
    uint32_t events = 0;

    // 根据类型设置读写事件
    switch (type) {
        case EpollEventType::READ:
            events = EPOLLIN;  // 监听可读事件
            break;
        case EpollEventType::WRITE:
            events = EPOLLOUT; // 监听可写事件
            break;
        case EpollEventType::READ_WRITE:
            events = EPOLLIN | EPOLLOUT; // 同时监听读写事件
            break;
    }

    // 添加边缘触发标志
    if (useEdgeTrigger) {
        events |= EPOLLET;
    }

    // 添加错误和断开连接事件（总是监听）
    events |= EPOLLERR | EPOLLHUP;

    return events;
}

/**
 * @brief 添加文件描述符到epoll监听
 *
 * 将socket注册到epoll实例，监听指定类型的事件。
 * 同时保存回调函数，用于事件触发时调用。
 *
 * @param fd 要监听的文件描述符
 * @param type 监听的事件类型
 * @param callback 事件触发时的回调函数
 * @param useEdgeTrigger 是否使用边缘触发模式
 * @return bool 添加成功返回true，失败返回false
 */
bool EpollManager::addFd(int fd, EpollEventType type, EpollEventCallback callback, bool useEdgeTrigger) {
    if (m_epollFd < 0) {
        LOG_ERROR("Epoll not created");
        return false;
    }

    // 保存回调函数
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_callbacks[fd] = callback;
    }

    // 准备epoll事件结构
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = convertEventType(type, useEdgeTrigger);
    ev.data.fd = fd;  // 将fd存储在data中，wait返回时可以获取

    /**
     * @brief 添加fd到epoll
     *
     * EPOLL_CTL_ADD: 添加新的fd到epoll监听集合
     * EPOLL_CTL_MOD: 修改已注册的fd的事件类型
     * EPOLL_CTL_DEL: 从epoll中删除fd
     */
    if (epoll_ctl(m_epollFd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        perror("epoll_ctl ADD failed");
        // 移除回调函数
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            m_callbacks.erase(fd);
        }
        return false;
    }

    LOG_DEBUG("Added fd " + std::to_string(fd) + " to epoll");
    return true;
}

/**
 * @brief 修改文件描述符的监听事件
 *
 * 修改已注册socket的监听事件类型，不改变回调函数
 *
 * @param fd 要修改的文件描述符
 * @param type 新的事件类型
 * @param useEdgeTrigger 是否使用边缘触发模式
 * @return bool 修改成功返回true，失败返回false
 */
bool EpollManager::modifyFd(int fd, EpollEventType type, bool useEdgeTrigger) {
    if (m_epollFd < 0) {
        LOG_ERROR("Epoll not created");
        return false;
    }

    // 准备epoll事件结构
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = convertEventType(type, useEdgeTrigger);
    ev.data.fd = fd;

    if (epoll_ctl(m_epollFd, EPOLL_CTL_MOD, fd, &ev) < 0) {
        perror("epoll_ctl MOD failed");
        return false;
    }

    LOG_DEBUG("Modified fd " + std::to_string(fd) + " events");
    return true;
}

/**
 * @brief 从epoll中删除文件描述符
 *
 * 停止监听指定socket的事件，移除回调函数
 *
 * @param fd 要删除的文件描述符
 * @return bool 删除成功返回true，失败返回false
 */
bool EpollManager::removeFd(int fd) {
    if (m_epollFd < 0) {
        return false;
    }

    // 从epoll中删除fd
    if (epoll_ctl(m_epollFd, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        // fd可能已经被关闭，不视为致命错误
        if (errno != EBADF) {
            perror("epoll_ctl DEL failed");
        }
    }

    // 移除回调函数
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_callbacks.erase(fd);
    }

    LOG_DEBUG("Removed fd " + std::to_string(fd) + " from epoll");
    return true;
}

/**
 * @brief 等待事件并分发处理
 *
 * 调用epoll_wait等待事件，然后调用注册的回调函数处理每个事件。
 * 这是epoll事件循环的核心函数。
 *
 * @param timeout 超时时间（毫秒）
 *                  -1: 无限等待，直到有事件发生
 *                   0: 非阻塞，立即返回
 *                  >0: 等待指定毫秒数
 * @return int 返回处理的事件数量，出错返回-1，超时返回0
 */
int EpollManager::waitAndDispatch(int timeout) {
    if (m_epollFd < 0) {
        LOG_ERROR("Epoll not created");
        return -1;
    }

    /**
     * @brief 等待事件
     *
     * epoll_wait参数说明：
     * - epfd: epoll文件描述符
     * - events: 事件数组，用于存储返回的事件
     * - maxevents: 最大事件数量（数组大小）
     * - timeout: 超时时间（毫秒）
     *
     * 返回值：
     * - >0: 返回的事件数量
     * -  0: 超时
     * - -1: 出错，errno被设置
     */
    int nfds = epoll_wait(m_epollFd, m_events, MAX_EVENTS, timeout);

    if (nfds < 0) {
        // 被信号中断，不算错误
        if (errno == EINTR) {
            return 0;
        }
        perror("epoll_wait failed");
        return -1;
    }

    if (nfds == 0) {
        // 超时，没有事件发生
        return 0;
    }

    // 分发处理每个事件
    for (int i = 0; i < nfds; ++i) {
        int fd = m_events[i].data.fd;
        uint32_t events = m_events[i].events;

        dispatchEvent(fd, events);
    }

    return nfds;
}

/**
 * @brief 处理单个事件
 *
 * 根据事件类型调用对应的回调函数。
 * 处理错误事件（EPOLLERR/EPOLLHUP）。
 *
 * @param fd 事件对应的文件描述符
 * @param events epoll返回的事件标志
 */
void EpollManager::dispatchEvent(int fd, uint32_t events) {
    EpollEventCallback callback;

    // 获取回调函数
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        auto it = m_callbacks.find(fd);
        if (it == m_callbacks.end()) {
            // 没有找到回调函数，可能是已被删除的fd
            LOG_WARN("No callback found for fd " + std::to_string(fd));
            return;
        }
        callback = it->second;
    }

    // 检查错误事件
    if (events & (EPOLLERR | EPOLLHUP)) {
        LOG_WARN("Error event on fd " + std::to_string(fd));
        // 仍然调用回调函数，让它处理错误情况
    }

    // 调用回调函数处理事件
    if (callback) {
        callback(fd, events);
    }
}
