/**
 * @file tcp_server.cc
 * @brief 可复用的TCP服务器基础类实现文件
 *
 * 该文件实现 TcpServer 的底层网络职责：
 * - 创建和关闭监听 socket
 * - 启动阻塞 accept 或 epoll 事件循环
 * - 接受新连接并通过回调交给上层
 * - 对 epoll 中的 fd 注册、修改和删除进行转发封装
 */

#include "tcp_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

TcpServer::TcpServer(const std::string& ip, int port, bool useEpoll)
    : m_ip(ip)
    , m_port(port)
    , m_useEpoll(useEpoll)
    , m_serverSocket(-1)
    , m_running(false) {
    signal(SIGPIPE, SIG_IGN);
}

/**
 * @brief 析构函数
 *
 * 依赖 stop() 完成资源释放，避免监听 socket 或后台线程泄漏。
 */
TcpServer::~TcpServer() {
    stop();
}

/**
 * @brief 更新监听地址
 */
void TcpServer::setAddress(const std::string& ip, int port) {
    m_ip = ip;
    m_port = port;
}

/**
 * @brief 更新运行模式
 */
void TcpServer::setUseEpoll(bool useEpoll) {
    m_useEpoll = useEpoll;
}

/**
 * @brief 设置新连接回调
 */
void TcpServer::setAcceptCallback(TcpAcceptCallback callback) {
    std::lock_guard<std::mutex> lock(m_acceptCallbackMutex);
    m_acceptCallback = std::move(callback);
}

/**
 * @brief 检查服务器是否正在运行
 */
bool TcpServer::isRunning() const {
    return m_running.load();
}

/**
 * @brief 检查当前是否启用了 epoll 模式
 */
bool TcpServer::isUsingEpoll() const {
    return m_useEpoll;
}

/**
 * @brief 获取监听 socket 描述符
 */
int TcpServer::getServerSocket() const {
    return m_serverSocket;
}

/**
 * @brief 创建监听 socket 并绑定地址
 *
 * 该步骤包含 socket 创建、端口复用、地址绑定和 listen 调用。
 * 任一步骤失败都会关闭已创建的 fd 并返回 false。
 */
bool TcpServer::createServerSocket() {
    m_serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverSocket < 0) {
        perror("socket");
        return false;
    }

    int opt = 1;
    if (setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
    }

    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(m_port);
    serverAddr.sin_addr.s_addr = inet_addr(m_ip.c_str());

    if (bind(m_serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("bind");
        close(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }

    if (listen(m_serverSocket, SOMAXCONN) < 0) {
        perror("listen");
        close(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }

    return true;
}

/**
 * @brief 启动 TCP 服务器
 *
 * epoll 模式下：
 * - 创建 epoll 实例
 * - 将监听 socket 设置为非阻塞
 * - 注册监听 socket 的读事件
 * - 启动 epoll 事件循环线程
 *
 * 非 epoll 模式下：
 * - 直接启动阻塞 accept 线程
 */
bool TcpServer::start() {
    if (m_running.load()) {
        return false;
    }

    if (!createServerSocket()) {
        return false;
    }

    m_running.store(true);

    if (m_useEpoll) {
        m_epollManager = std::make_unique<EpollManager>();
        if (!m_epollManager->create()) {
            close(m_serverSocket);
            m_serverSocket = -1;
            m_running.store(false);
            return false;
        }

        if (!setNonBlocking(m_serverSocket)) {
            m_epollManager->closeEpoll();
            m_epollManager.reset();
            close(m_serverSocket);
            m_serverSocket = -1;
            m_running.store(false);
            return false;
        }

        auto serverCallback = [this](int fd, uint32_t events) {
            this->handleServerRead(fd, events);
        };
        if (!m_epollManager->addFd(m_serverSocket, EpollEventType::READ, serverCallback, false)) {
            m_epollManager->closeEpoll();
            m_epollManager.reset();
            close(m_serverSocket);
            m_serverSocket = -1;
            m_running.store(false);
            return false;
        }

        m_epollThread = std::thread(&TcpServer::epollEventLoop, this);
    } else {
        m_acceptThread = std::thread(&TcpServer::acceptConnections, this);
    }

    return true;
}

/**
 * @brief 停止 TCP 服务器
 *
 * 该函数会停止事件循环、关闭监听 socket 并等待后台线程退出。
 * 已接受的连接是否关闭由上层决定。
 */
void TcpServer::stop() {
    if (!m_running.load()) {
        return;
    }

    m_running.store(false);

    if (m_serverSocket >= 0) {
        close(m_serverSocket);
        m_serverSocket = -1;
    }

    if (m_useEpoll) {
        if (m_epollManager) {
            m_epollManager->stop();
        }

        if (m_epollThread.joinable()) {
            m_epollThread.join();
        }

        if (m_epollManager) {
            m_epollManager->closeEpoll();
            m_epollManager.reset();
        }
    } else {
        if (m_acceptThread.joinable()) {
            m_acceptThread.join();
        }
    }
}

/**
 * @brief 注册一个 fd 到 epoll 管理器
 */
bool TcpServer::addFd(int fd, EpollEventType type, EpollEventCallback callback, bool useEdgeTrigger) {
    if (!m_epollManager) {
        return false;
    }
    return m_epollManager->addFd(fd, type, callback, useEdgeTrigger);
}

/**
 * @brief 修改 epoll 中已有 fd 的监听类型
 */
bool TcpServer::modifyFd(int fd, EpollEventType type, bool useEdgeTrigger) {
    if (!m_epollManager) {
        return false;
    }
    return m_epollManager->modifyFd(fd, type, useEdgeTrigger);
}

/**
 * @brief 从 epoll 中移除 fd
 */
bool TcpServer::removeFd(int fd) {
    if (!m_epollManager) {
        return false;
    }
    return m_epollManager->removeFd(fd);
}

/**
 * @brief 将 socket 设置为非阻塞模式
 */
bool TcpServer::setNonBlocking(int fd) {
    return EpollManager::setNonBlocking(fd);
}

/**
 * @brief 非 epoll 模式下的阻塞 accept 循环
 */
void TcpServer::acceptConnections() {
    while (m_running.load()) {
        if (!acceptClientConnection()) {
            break;
        }
    }
}

/**
 * @brief epoll 事件循环
 *
 * 通过 epoll_wait 等待事件，并将事件交给 EpollManager 自行分发。
 */
void TcpServer::epollEventLoop() {
    while (m_running.load() && m_epollManager && m_epollManager->isCreated()) {
        int nfds = m_epollManager->waitAndDispatch(100);
        if (nfds < 0) {
            break;
        }
    }
}

/**
 * @brief 处理监听 socket 的可读事件
 *
 * 当监听 socket 收到可读事件时，说明有新连接到达。该函数会继续调用
 * acceptClientConnection()，直到当前可接受的连接被取尽。
 */
void TcpServer::handleServerRead(int serverSocket, uint32_t events) {
    static_cast<void>(serverSocket);

    if (events & (EPOLLERR | EPOLLHUP)) {
        return;
    }

    while (m_running.load() && acceptClientConnection()) {
    }
}

/**
 * @brief 接受一个新的客户端连接
 *
 * 连接建立成功后：
 * - 提取客户端 IP 和端口
 * - 取出当前回调
 * - 将新连接交给上层
 *
 * 如果没有注册回调，则关闭该连接以避免 fd 泄漏。
 */
bool TcpServer::acceptClientConnection() {
    struct sockaddr_in clientAddr;
    socklen_t clientAddrLen = sizeof(clientAddr);

    int clientSocket = accept(m_serverSocket, (struct sockaddr*)&clientAddr, &clientAddrLen);
    if (clientSocket < 0) {
        if (errno == EINTR) {
            return true;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return false;
        }
        perror("accept");
        return false;
    }

    char clientIp[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, sizeof(clientIp));
    int clientPort = ntohs(clientAddr.sin_port);

    TcpAcceptCallback callback;
    {
        std::lock_guard<std::mutex> lock(m_acceptCallbackMutex);
        callback = m_acceptCallback;
    }

    if (callback) {
        callback(clientSocket, std::string(clientIp), clientPort);
        return true;
    }

    close(clientSocket);
    return true;
}