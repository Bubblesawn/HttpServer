/**
 * @file epoll_manager.h
 * @brief epoll事件管理器头文件
 *
 * 本文件定义了EpollManager类，提供基于epoll的IO多路复用功能。
 * 使用epoll替代传统的one-thread-per-connection模型，支持高并发连接。
 */

#ifndef EPOLL_MANAGER_H
#define EPOLL_MANAGER_H

#include <atomic>       // 原子操作
#include <cerrno>       // 错误码
#include <cstring>      // C字符串处理
#include <fcntl.h>      // 文件控制选项
#include <functional>   // 函数对象
#include <mutex>        // 互斥锁
#include <sys/epoll.h>  // epoll系统调用
#include <sys/socket.h> // socket编程接口
#include <unistd.h>     // POSIX API
#include <vector>       // 向量容器

#include "../logger/logger.h"

/**
 * @brief epoll事件类型枚举
 *
 * 定义epoll关注的事件类型，用于注册socket事件
 */
enum class EpollEventType {
  READ,      // 可读事件
  WRITE,     // 可写事件
  READ_WRITE // 可读可写事件
};

/**
 * @brief epoll事件回调函数类型
 *
 * 当epoll检测到事件时，调用此回调函数处理
 * @param fd 触发事件的文件描述符
 * @param events epoll返回的事件类型（EPOLLIN/EPOLLOUT/EPOLLERR等）
 */
using EpollEventCallback = std::function<void(int fd, uint32_t events)>;

/**
 * @brief epoll事件管理器类
 *
 * 封装epoll的创建、事件注册、事件等待等功能。
 * 提供线程安全的事件管理接口。
 *
 * 主要功能：
 * - 创建和管理epoll实例
 * - 注册/修改/删除socket事件监听
 * - 等待并分发事件
 * - 设置socket为非阻塞模式
 *
 * 使用示例：
 * @code
 * EpollManager epoll;
 * epoll.create();
 * epoll.addFd(serverSocket, EpollEventType::READ, onAccept);
 * epoll.waitAndDispatch(-1);  // 阻塞等待事件
 * @endcode
 */
class EpollManager {
public:
  /**
   * @brief 构造函数
   *
   * 初始化epoll管理器，设置默认参数
   */
  EpollManager();

  /**
   * @brief 析构函数
   *
   * 自动释放epoll资源
   */
  ~EpollManager();

  /**
   * @brief 创建epoll实例
   *
   * 调用epoll_create1创建epoll文件描述符
   *
   * @return bool 创建成功返回true，失败返回false
   */
  bool create();

  /**
   * @brief 关闭epoll实例
   *
   * 关闭epoll文件描述符，释放资源
   */
  void closeEpoll();

  /**
   * @brief 添加文件描述符到epoll监听
   *
   * 将socket注册到epoll，监听指定类型的事件
   *
   * @param fd 要监听的文件描述符
   * @param type 监听的事件类型（读/写/读写）
   * @param callback 事件触发时的回调函数
   * @param useEdgeTrigger 是否使用边缘触发模式（默认true）
   * @return bool 添加成功返回true，失败返回false
   *
   * @note 边缘触发(ET)模式下，必须一次性读完所有数据，否则可能丢失事件
   */
  bool addFd(int fd, EpollEventType type, EpollEventCallback callback,
             bool useEdgeTrigger = true);

  /**
   * @brief 修改文件描述符的监听事件
   *
   * 修改已注册socket的监听事件类型
   *
   * @param fd 要修改的文件描述符
   * @param type 新的事件类型
   * @param useEdgeTrigger 是否使用边缘触发模式
   * @return bool 修改成功返回true，失败返回false
   */
  bool modifyFd(int fd, EpollEventType type, bool useEdgeTrigger = true);

  /**
   * @brief 从epoll中删除文件描述符
   *
   * 停止监听指定socket的事件
   *
   * @param fd 要删除的文件描述符
   * @return bool 删除成功返回true，失败返回false
   */
  bool removeFd(int fd);

  /**
   * @brief 等待事件并分发处理
   *
   * 调用epoll_wait等待事件，然后调用注册的回调函数处理
   *
   * @param timeout 超时时间（毫秒），-1表示无限等待，0表示非阻塞
   * @return int 返回处理的事件数量，出错返回-1
   */
  int waitAndDispatch(int timeout);

  /**
   * @brief 设置socket为非阻塞模式
   *
   * 使用fcntl设置O_NONBLOCK标志
   *
   * @param fd 要设置的socket文件描述符
   * @return bool 设置成功返回true，失败返回false
   */
  static bool setNonBlocking(int fd);

  /**
   * @brief 检查epoll是否已创建
   *
   * @return bool epoll已创建返回true，否则返回false
   */
  bool isCreated() const { return m_epollFd >= 0; }

  /**
   * @brief 获取epoll文件描述符
   *
   * @return int epoll文件描述符，未创建返回-1
   */
  int getEpollFd() const { return m_epollFd; }

  /**
   * @brief 停止epoll事件循环
   *
   * 设置停止标志，使waitAndDispatch退出
   */
  void stop() { m_running.store(false); }

private:
  /**
   * @brief 将EpollEventType转换为epoll事件标志
   *
   * @param type 事件类型枚举
   * @param useEdgeTrigger 是否使用边缘触发
   * @return uint32_t epoll事件标志位组合
   */
  uint32_t convertEventType(EpollEventType type, bool useEdgeTrigger);

  /**
   * @brief 处理单个事件
   *
   * @param fd 事件对应的文件描述符
   * @param events epoll返回的事件标志
   */
  void dispatchEvent(int fd, uint32_t events);

private:
  int m_epollFd;                           // epoll文件描述符
  static constexpr int MAX_EVENTS = 1024;  // 最大事件数量
  struct epoll_event m_events[MAX_EVENTS]; // 事件数组
  std::atomic<bool> m_running;             // 运行标志
  std::mutex m_callbackMutex;              // 回调函数映射的互斥锁
  std::unordered_map<int, EpollEventCallback> m_callbacks; // fd到回调函数的映射
};

#endif // EPOLL_MANAGER_H
