/**
 * @file tcp_server.h
 * @brief 可复用的TCP服务器基础类头文件
 *
 * 该类只负责监听socket、连接接受、原生socket事件分发，
 * 不关心上层协议细节。上层可通过回调函数接收连接与事件。
 */

#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "epoll_manager.h"

/**
 * @brief TCP连接接受回调
 *
 * 当TCP层成功接受到一个新连接后触发。
 *
 * 上层协议可以在这个回调中完成以下工作：
 * - 记录连接来源信息
 * - 配置连接的 socket 选项
 * - 将连接注册到自己的事件循环
 * - 把连接交给线程池或业务处理器
 *
 * @param fd 新接受到的客户端 socket 描述符
 * @param ip 客户端 IP 地址字符串
 * @param port 客户端端口号
 */
using TcpAcceptCallback = std::function<void(int fd, const std::string& ip, int port)>;

/**
 * @brief TCP服务器基础类
 *
 * 该类只负责最底层的 TCP 传输职责，不关心 HTTP、RPC 或其他应用协议。
 * 它提供以下能力：
 * - 创建并管理监听 socket
 * - 接受新的客户端连接
 * - 将原生 socket 事件分发给注册的回调
 * - 在 epoll / 阻塞 accept 两种模式之间切换
 *
 * 上层可以通过连接回调和 fd 回调接管具体业务处理。
 */
class TcpServer {
public:
    /**
     * @brief 构造函数
     *
     * 初始化 TCP 服务器的绑定地址、监听端口和运行模式。
     *
     * @param ip 监听的 IP 地址，默认监听所有网卡
     * @param port 监听端口
     * @param useEpoll 是否使用 epoll 事件循环
     */
    TcpServer(const std::string& ip = "0.0.0.0", int port = 8080, bool useEpoll = true);

    /**
     * @brief 析构函数
     *
     * 自动停止服务器并释放监听 socket、epoll 资源和线程资源。
     */
    ~TcpServer();

    /**
     * @brief 启动 TCP 服务器
     *
     * 创建监听 socket，绑定地址并进入监听状态；
     * 在 epoll 模式下还会创建 epoll 实例并启动事件循环线程。
     *
     * @return bool 启动成功返回 true，失败返回 false
     */
    bool start();

    /**
     * @brief 停止 TCP 服务器
     *
     * 关闭监听 socket，停止事件循环，并等待工作线程退出。
     * 已经接受的连接不会被自动关闭，是否关闭由上层决定。
     */
    void stop();

    /**
     * @brief 检查服务器是否正在运行
     *
     * @return bool 正在运行返回 true，否则返回 false
     */
    bool isRunning() const;

    /**
     * @brief 检查当前是否启用了 epoll 模式
     *
     * @return bool 使用 epoll 返回 true，否则返回 false
     */
    bool isUsingEpoll() const;

    /**
     * @brief 更新监听地址
     *
     * 该设置只在 start() 之前生效。
     *
     * @param ip 新的监听 IP
     * @param port 新的监听端口
     */
    void setAddress(const std::string& ip, int port);

    /**
     * @brief 设置运行模式
     *
     * 该设置只在 start() 之前生效。
     *
     * @param useEpoll true 表示使用 epoll，false 表示使用阻塞 accept
     */
    void setUseEpoll(bool useEpoll);

    /**
     * @brief 设置新连接回调
     *
     * 当 TCP 层接受到新连接时触发该回调。
     *
     * @param callback 新连接回调函数
     */
    void setAcceptCallback(TcpAcceptCallback callback);

    /**
     * @brief 注册一个 fd 到 epoll 管理器
     *
     * 这是对 EpollManager 的转发封装，上层可以直接通过 TCP 服务器对象
     * 管理已接受连接的监听事件。
     *
     * @param fd 要监听的文件描述符
     * @param type 监听的事件类型
     * @param callback 事件触发时执行的回调函数
     * @param useEdgeTrigger 是否启用边缘触发模式，默认开启
     * @return bool 注册成功返回 true，失败返回 false
     */
    bool addFd(int fd, EpollEventType type, EpollEventCallback callback, bool useEdgeTrigger = true);

    /**
     * @brief 修改 epoll 中已有 fd 的监听类型
     *
     * @param fd 要修改的文件描述符
     * @param type 新的事件类型
     * @param useEdgeTrigger 是否启用边缘触发模式
     * @return bool 修改成功返回 true，失败返回 false
     */
    bool modifyFd(int fd, EpollEventType type, bool useEdgeTrigger = true);

    /**
     * @brief 从 epoll 中移除 fd
     *
     * @param fd 要移除的文件描述符
     * @return bool 移除成功返回 true，失败返回 false
     */
    bool removeFd(int fd);

    /**
     * @brief 将 socket 设置为非阻塞模式
     *
     * 这是对 EpollManager::setNonBlocking 的静态转发，方便上层统一调用。
     *
     * @param fd 要修改的 socket 文件描述符
     * @return bool 设置成功返回 true，失败返回 false
     */
    static bool setNonBlocking(int fd);

    /**
     * @brief 获取监听 socket 描述符
     *
     * @return int 当前监听 socket，未创建时返回 -1
     */
    int getServerSocket() const;

private:
    /**
     * @brief 创建监听 socket 并绑定监听地址
     *
     * 负责 socket、setsockopt、bind 和 listen 的完整初始化过程。
     *
     * @return bool 成功返回 true，失败返回 false
     */
    bool createServerSocket();

    /**
     * @brief 阻塞式接受连接循环
     *
     * 在非 epoll 模式下使用，持续调用 acceptClientConnection()。
     */
    void acceptConnections();

    /**
     * @brief epoll 事件循环
     *
     * 在 epoll 模式下运行，持续等待并分发监听 socket 与连接 socket 事件。
     */
    void epollEventLoop();

    /**
     * @brief 处理监听 socket 的可读事件
     *
     * 当监听 socket 可读时，说明有新连接到来；该函数会尽可能接受完
     * 当前可用的新连接。
     *
     * @param serverSocket 监听 socket 描述符
     * @param events epoll 返回的事件标志
     */
    void handleServerRead(int serverSocket, uint32_t events);

    /**
     * @brief 接受一个新的客户端连接
     *
     * 接受成功后会通过 accept 回调将 socket 和地址信息交给上层。
     *
     * @return bool 成功接受或可继续重试返回 true，没有更多连接或发生错误返回 false
     */
    bool acceptClientConnection();

private:
    /** 监听 IP 地址 */
    std::string m_ip;

    /** 监听端口 */
    int m_port;

    /** 是否启用 epoll 模式 */
    bool m_useEpoll;

    /** 监听 socket 描述符 */
    int m_serverSocket;

    /** 服务器运行状态标志 */
    std::atomic<bool> m_running;

    /** epoll 管理器 */
    std::unique_ptr<EpollManager> m_epollManager;

    /** 阻塞 accept 模式下的接受线程 */
    std::thread m_acceptThread;

    /** epoll 模式下的事件循环线程 */
    std::thread m_epollThread;

    /** 新连接回调 */
    TcpAcceptCallback m_acceptCallback;

    /** 新连接回调的互斥锁 */
    std::mutex m_acceptCallbackMutex;
};

#endif // TCP_SERVER_H