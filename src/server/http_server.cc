/**
 * @file http_server.cc
 * @brief HTTP服务器类的实现文件
 * 
 * 实现HttpServer类中声明的所有方法，
 * 包括服务器启动、停止、请求处理、静态文件服务等功能。
 */

#include "../server/http_server.h"
#include "../request/http_request.h"
#include "../response/http_response.h"
#include "../thread/thread_pool.h"
#include "../cache/file_cache.h"
#include <thread>

// POSIX网络编程头文件
#include <sys/socket.h>      // socket编程接口
#include <sys/types.h>       // 数据类型定义
#include <sys/stat.h>        // 文件状态
#include <sys/sendfile.h>    // sendfile零拷贝
#include <netinet/in.h>     // 网络地址结构
#include <netinet/tcp.h>    // TCP协议选项
#include <arpa/inet.h>      // IP地址转换
#include <unistd.h>          // POSIX API (close, read, write等)
#include <fcntl.h>           // 文件控制
#include <signal.h>          // 信号处理
#include <dirent.h>         // 目录操作
#include <cstring>           // C字符串处理
#include <cerrno>           // 错误处理
#include <fstream>           // 文件流
#include <sstream>           // 字符串流
#include <algorithm>         // 算法
#include <cctype>           // 字符处理
#include <iostream>          // 输入输出

// 为兼容旧版本系统，定义EPOLLRDHUP（如果未定义）
#ifndef EPOLLRDHUP
#define EPOLLRDHUP 0x2000
#endif

/**
 * @brief 构造函数
 * 
 * 初始化HttpServer对象，设置默认配置。
 * 
 * @param ip 服务器绑定的IP地址（默认"0.0.0.0"）
 * @param port 服务器监听端口（默认8080）
 */
HttpServer::HttpServer(const std::string& ip, int port)
    : m_ip(ip)
    , m_port(port)
    , m_docRoot("./html_docs")    // 默认文档根目录
    , m_numThreads(std::thread::hardware_concurrency() * 2)  // 默认线程数为CPU核心数的2倍，提高并发处理能力
    , m_serverSocket(-1)         // 初始化为无效socket
    , m_running(false)            // 初始状态为未运行
    , m_useEpoll(true)           // 默认启用epoll模式
    , m_fileCache(nullptr) {     // 文件缓存初始化为nullptr
    /**
     * @brief 忽略SIGPIPE信号
     *
     * 当向已关闭的socket写入数据时，进程会收到SIGPIPE信号并终止。
     * 忽略该信号可以防止这种情况，让write返回错误码而不是终止进程。
     *
     * 常见场景：客户端提前关闭连接，但服务器仍在发送数据
     */
    signal(SIGPIPE, SIG_IGN);
}

/**
 * @brief 析构函数
 * 
 * 确保服务器被正确停止，释放所有资源。
 */
HttpServer::~HttpServer() {
    stop();  // 调用stop确保资源释放
}

/**
 * @brief 启动HTTP服务器
 * 
 * 执行以下步骤：
 * 1. 检查服务器是否已在运行
 * 2. 创建服务器socket
 * 3. 设置socket选项（地址重用）
 * 4. 绑定地址和端口
 * 5. 开始监听连接
 * 6. 初始化线程池
 * 
 * @return bool 启动成功返回true，失败返回false
 */
bool HttpServer::start() {
    // 检查服务器是否已在运行
    if (m_running.load()) {
        return false;
    }

    // 创建TCP socket
    // AF_INET: IPv4协议
    // SOCK_STREAM: 面向连接的可靠数据传输（TCP）
    m_serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverSocket < 0) {
        perror("socket");  // 输出错误信息到stderr
        return false;
    }

    /**
     * @brief 设置socket选项 - 地址重用
     * 
     * SO_REUSEADDR允许在服务器关闭后立即重新绑定到相同端口，
     * 而不需要等待操作系统释放端口（通常有TIME_WAIT状态）。
     * 
     * 这在开发调试时特别有用，可以快速重启服务器。
     */
    int opt = 1;
    if (setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
    }

    // 准备服务器地址结构
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));  // 清零结构体
    
    serverAddr.sin_family = AF_INET;                    // IPv4
    serverAddr.sin_port = htons(m_port);               // 端口号（主机字节序转网络字节序）
    serverAddr.sin_addr.s_addr = inet_addr(m_ip.c_str());  // IP地址

    // 绑定地址和端口到socket
    if (bind(m_serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("bind");
        close(m_serverSocket);      // 绑定失败，关闭socket
        m_serverSocket = -1;
        return false;
    }

    // 开始监听连接请求
    // SOMAXCONN: 使用系统允许的最大等待队列长度，支持高并发连接
    if (listen(m_serverSocket, SOMAXCONN) < 0) {
        perror("listen");
        close(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }

    // 设置服务器为运行状态
    m_running.store(true);

    // 根据模式选择启动方式
    if (m_useEpoll) {
        // ========== epoll模式 ==========
        // 创建线程池（用于处理请求）
        m_threadPool = std::make_unique<ThreadPool>(m_numThreads);

        // 创建文件缓存
        m_fileCache = std::make_unique<FileCache>();

        // 创建epoll管理器
        m_epollManager = std::make_unique<EpollManager>();
        if (!m_epollManager->create()) {
            LOG_ERROR("Failed to create epoll instance");
            close(m_serverSocket);
            m_serverSocket = -1;
            m_running.store(false);
            return false;
        }

        // 设置服务器socket为非阻塞模式
        if (!EpollManager::setNonBlocking(m_serverSocket)) {
            LOG_ERROR("Failed to set server socket to non-blocking mode");
            m_epollManager->closeEpoll();
            close(m_serverSocket);
            m_serverSocket = -1;
            m_running.store(false);
            return false;
        }

        // 添加服务器socket到epoll监听（水平触发模式，避免遗漏连接）
        auto serverCallback = [this](int fd, uint32_t events) {
            this->handleServerRead(fd, events);
        };
        if (!m_epollManager->addFd(m_serverSocket, EpollEventType::READ, serverCallback, false)) {
            LOG_ERROR("Failed to add server socket to epoll");
            m_epollManager->closeEpoll();
            close(m_serverSocket);
            m_serverSocket = -1;
            m_running.store(false);
            return false;
        }

        // 启动epoll事件处理线程
        m_epollThread = std::thread(&HttpServer::epollEventLoop, this);

        LOG_INFO("Server started on " + m_ip + ":" + std::to_string(m_port));
        LOG_INFO("Document root: " + m_docRoot);
        LOG_INFO("Thread pool size: " + std::to_string(m_numThreads));
        LOG_INFO("Mode: epoll + thread pool (hybrid)");
    } else {
        // ========== 传统线程池模式 ==========
        // 创建线程池
        m_threadPool = std::make_unique<ThreadPool>(m_numThreads);

        // 创建文件缓存
        m_fileCache = std::make_unique<FileCache>();

        // 启动接受连接的线程
        m_acceptThread = std::thread(&HttpServer::acceptConnections, this);

        LOG_INFO("Server started on " + m_ip + ":" + std::to_string(m_port));
        LOG_INFO("Document root: " + m_docRoot);
        LOG_INFO("Thread pool size: " + std::to_string(m_numThreads));
        LOG_INFO("Mode: thread pool (one-thread-per-connection)");
    }

    // 短暂等待让线程启动
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    return true;
}

/**
 * @brief 停止HTTP服务器
 * 
 * 优雅停止服务器：
 * 1. 设置停止标志
 * 2. 关闭服务器socket（停止接受新连接）
 * 3. 关闭线程池（等待所有任务完成）
 * 
 * 注意：已建立的连接将继续处理直到完成
 */
void HttpServer::stop() {
    // 检查服务器是否在运行
    if (!m_running.load()) {
        return;
    }

    // 设置停止标志
    m_running.store(false);

    // 关闭服务器socket，停止接受新连接
    if (m_serverSocket >= 0) {
        close(m_serverSocket);
        m_serverSocket = -1;
    }

    if (m_useEpoll) {
        // ========== epoll模式停止 ==========
        // 停止epoll事件循环
        if (m_epollManager) {
            m_epollManager->stop();
        }

        // 等待epoll事件处理线程结束
        if (m_epollThread.joinable()) {
            m_epollThread.join();
        }

        // 关闭线程池，等待所有任务完成
        if (m_threadPool) {
            m_threadPool->shutdown();
            m_threadPool.reset();
        }

        // 关闭文件缓存
        if (m_fileCache) {
            m_fileCache.reset();
        }

        // 关闭所有客户端连接
        {
            std::lock_guard<std::mutex> lock(m_clientInfoMutex);
            for (auto& pair : m_clientInfoMap) {
                close(pair.first);
            }
            m_clientInfoMap.clear();
        }

        // 关闭epoll管理器
        if (m_epollManager) {
            m_epollManager->closeEpoll();
            m_epollManager.reset();
        }
    } else {
        // ========== 传统线程池模式停止 ==========
        // 等待接受连接的线程结束
        if (m_acceptThread.joinable()) {
            m_acceptThread.join();
        }

        // 关闭线程池，等待所有任务完成
        if (m_threadPool) {
            m_threadPool->shutdown();
            m_threadPool.reset();  // 释放智能指针
        }
    }

    LOG_INFO("Server stopped");
}

/**
 * @brief 检查服务器是否正在运行
 * 
 * @return bool 运行返回true，否则返回false
 */
bool HttpServer::isRunning() const {
    return m_running.load();
}

/**
 * @brief 设置服务器端口
 * 
 * @param port 端口号
 */
void HttpServer::setPort(int port) {
    m_port = port;
}

/**
 * @brief 获取服务器端口
 * 
 * @return int 当前端口号
 */
int HttpServer::getPort() const {
    return m_port;
}

/**
 * @brief 设置文档根目录
 * 
 * @param docRoot 目录路径
 */
void HttpServer::setDocRoot(const std::string& docRoot) {
    m_docRoot = docRoot;
}

/**
 * @brief 获取文档根目录
 * 
 * @return std::string 文档根目录
 */
std::string HttpServer::getDocRoot() const {
    return m_docRoot;
}

/**
 * @brief 设置线程池大小
 * 
 * @param numThreads 线程数量
 */
void HttpServer::setNumThreads(int numThreads) {
    m_numThreads = numThreads;
}

/**
 * @brief 获取线程池大小
 * 
 * @return int 线程数量
 */
int HttpServer::getNumThreads() const {
    return m_numThreads;
}

/**
 * @brief 启用或禁用静态文件缓存
 *
 * @param enabled true启用缓存，false禁用缓存
 */
void HttpServer::setCacheEnabled(bool enabled) {
    if (m_fileCache) {
        m_fileCache->setEnabled(enabled);
    }
}

/**
 * @brief 检查缓存是否启用
 *
 * @return bool 缓存启用返回true，否则返回false
 */
bool HttpServer::isCacheEnabled() const {
    if (m_fileCache) {
        return m_fileCache->isEnabled();
    }
    return false;
}

/**
 * @brief 设置最大缓存大小
 *
 * @param maxSize 最大缓存大小（字节）
 */
void HttpServer::setCacheMaxSize(size_t maxSize) {
    if (m_fileCache) {
        m_fileCache->setMaxSize(maxSize);
    }
}

/**
 * @brief 获取最大缓存大小
 *
 * @return size_t 最大缓存大小（字节）
 */
size_t HttpServer::getCacheMaxSize() const {
    if (m_fileCache) {
        return m_fileCache->getMaxSize();
    }
    return 0;
}

/**
 * @brief 设置单文件最大缓存大小
 *
 * @param maxSize 单文件最大缓存大小（字节）
 */
void HttpServer::setCacheMaxFileSize(size_t maxSize) {
    if (m_fileCache) {
        m_fileCache->setMaxFileSize(maxSize);
    }
}

/**
 * @brief 获取单文件最大缓存大小
 *
 * @return size_t 单文件最大缓存大小（字节）
 */
size_t HttpServer::getCacheMaxFileSize() const {
    if (m_fileCache) {
        return m_fileCache->getMaxFileSize();
    }
    return 0;
}

/**
 * @brief 获取缓存统计信息
 *
 * @return std::string 缓存统计信息的JSON格式字符串
 */
std::string HttpServer::getCacheStats() const {
    if (!m_fileCache) {
        return "{}";
    }

    std::ostringstream oss;
    oss << "{";
    oss << "\"enabled\":" << (m_fileCache->isEnabled() ? "true" : "false") << ",";
    oss << "\"maxSize\":" << m_fileCache->getMaxSize() << ",";
    oss << "\"currentSize\":" << m_fileCache->getCurrentSize() << ",";
    oss << "\"maxFileSize\":" << m_fileCache->getMaxFileSize() << ",";
    oss << "\"cacheCount\":" << m_fileCache->getCacheCount() << ",";
    oss << "\"hitCount\":" << m_fileCache->getHitCount() << ",";
    oss << "\"missCount\":" << m_fileCache->getMissCount() << ",";
    oss << "\"hitRate\":" << m_fileCache->getHitRate();
    oss << "}";

    return oss.str();
}

/**
 * @brief 清空文件缓存
 */
void HttpServer::clearCache() {
    if (m_fileCache) {
        m_fileCache->clear();
    }
}

/**
 * @brief 设置自定义请求处理函数
 * 
 * @param handler 请求处理函数
 */
void HttpServer::setRequestHandler(RequestHandler handler) {
    m_requestHandler = handler;
}

/**
 * @brief 获取本地IP地址
 * 
 * 通过getsockname获取服务器绑定的实际IP地址。
 * 
 * @return std::string IP地址字符串
 */
std::string HttpServer::getLocalIp() const {
    char ip[INET_ADDRSTRLEN];  // INET_ADDRSTRLEN (16) 足够存储IPv4地址字符串
    struct sockaddr_in addr;
    socklen_t addrLen = sizeof(addr);
    
    // 获取socket绑定的地址信息
    if (getsockname(m_serverSocket, (struct sockaddr*)&addr, &addrLen) == 0) {
        // 将网络字节序的IP地址转换为字符串格式
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        return std::string(ip);
    }
    
    return "0.0.0.0";  // 出错时返回默认值
}

/**
 * @brief 接受客户端连接
 * 
 * 在独立线程中运行，持续接受新的客户端连接。
 * 每个新连接被包装成任务提交到线程池。
 * 
 * 工作流程：
 * 1. 等待客户端连接（accept阻塞）
 * 2. 获取客户端地址信息
 * 3. 将处理任务提交到线程池
 * 4. 继续等待下一个连接
 */
void HttpServer::acceptConnections() {
    // 持续接受连接直到服务器停止
    while (m_running.load()) {
        struct sockaddr_in clientAddr;
        socklen_t clientAddrLen = sizeof(clientAddr);

        /**
         * @brief 接受客户端连接
         * 
         * accept()会阻塞直到有客户端连接到来。
         * 成功时返回一个新的socket描述符用于与该客户端通信。
         * 
         * @note 这里的clientSocket是独立的，与m_serverSocket不同
         */
        int clientSocket = accept(m_serverSocket, 
                                 (struct sockaddr*)&clientAddr, 
                                 &clientAddrLen);
        
        if (clientSocket < 0) {
            // 被中断信号打断，继续等待
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;  // 其他错误，退出循环
        }

        // 获取客户端IP地址和端口
        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, sizeof(clientIp));
        int clientPort = ntohs(clientAddr.sin_port);  // 网络字节序转主机字节序

        std::cout << "Client connected: " << clientIp << ":" << clientPort << std::endl;

        /**
         * @brief 将客户端处理任务加入线程池
         * 
         * 使用lambda表达式捕获this指针和参数，
         * 将处理任务提交到线程池的工作队列。
         * 
         * 线程池会自动分配空闲线程来执行这个任务。
         */
        m_threadPool->enqueue([this, clientSocket, clientIp, clientPort]() {
            handleClient(clientSocket, clientIp, clientPort);
        });
    }
}

/**
 * @brief epoll事件循环
 *
 * 在独立线程中运行，持续调用epoll_wait等待并处理IO事件。
 * 这是epoll模式的核心事件循环。
 */
void HttpServer::epollEventLoop() {
    LOG_INFO("Epoll event loop started");

    while (m_running.load() && m_epollManager && m_epollManager->isCreated()) {
        // 等待事件，超时时间100ms（用于定期检查running状态）
        int nfds = m_epollManager->waitAndDispatch(100);

        if (nfds < 0) {
            LOG_ERROR("Epoll wait error");
            break;
        }

        // 可以在这里添加额外的处理逻辑，如定时任务等
    }

    LOG_INFO("Epoll event loop stopped");
}

/**
 * @brief 处理服务器socket可读事件（epoll模式）
 *
 * 当epoll检测到服务器socket可读时调用，表示有新连接到来。
 * 接受所有可用的新连接（非阻塞模式可能一次有多个）。
 *
 * @param serverSocket 服务器socket描述符
 * @param events epoll事件标志
 */
void HttpServer::handleServerRead(int serverSocket, uint32_t events) {
    // 检查错误事件
    if (events & (EPOLLERR | EPOLLHUP)) {
        LOG_ERROR("Error on server socket");
        return;
    }

    // 接受所有可用的新连接（非阻塞accept）
    while (m_running.load()) {
        struct sockaddr_in clientAddr;
        socklen_t clientAddrLen = sizeof(clientAddr);

        int clientSocket = accept(serverSocket,
                                 (struct sockaddr*)&clientAddr,
                                 &clientAddrLen);

        if (clientSocket < 0) {
            // EAGAIN/EWOULDBLOCK表示没有更多连接了
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            // EINTR表示被信号中断，继续尝试
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }

        // 获取客户端IP地址和端口
        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, sizeof(clientIp));
        int clientPort = ntohs(clientAddr.sin_port);

        LOG_INFO("Client connected: " + std::string(clientIp) + ":" + std::to_string(clientPort));

        // 设置客户端socket为非阻塞模式
        if (!EpollManager::setNonBlocking(clientSocket)) {
            LOG_ERROR("Failed to set client socket to non-blocking mode");
            close(clientSocket);
            continue;
        }

        // 设置socket接收和发送超时时间（5秒），防止客户端长时间占用连接
        struct timeval timeout;
        timeout.tv_sec = 5;   // 5秒超时
        timeout.tv_usec = 0;
        setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        // 启用TCP_NODELAY，禁用Nagle算法，减少小数据包延迟
        int nodelay = 1;
        setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        // 保存客户端信息
        {
            std::lock_guard<std::mutex> lock(m_clientInfoMutex);
            m_clientInfoMap[clientSocket] = {std::string(clientIp), clientPort, ""};
        }

        // 添加客户端socket到epoll监听（水平触发模式，简化处理逻辑）
        auto clientCallback = [this](int fd, uint32_t ev) {
            this->handleClientRead(fd, ev);
        };

        if (!m_epollManager->addFd(clientSocket, EpollEventType::READ, clientCallback, false)) {
            LOG_ERROR("Failed to add client socket to epoll");
            close(clientSocket);
            std::lock_guard<std::mutex> lock(m_clientInfoMutex);
            m_clientInfoMap.erase(clientSocket);
            continue;
        }
    }
}

/**
 * @brief 处理客户端可读事件（epoll模式）
 *
 * 当epoll检测到客户端socket可读时调用。
 * 将请求提交到线程池处理，实现IO事件通知与请求处理的分离。
 *
 * 注意：使用水平触发(LT)模式，epoll会重复通知直到数据处理完毕。
 * 这里采用"一个连接由一个线程处理"的方式，处理完即关闭连接。
 *
 * @param clientSocket 客户端socket描述符
 * @param events epoll事件标志
 */
void HttpServer::handleClientRead(int clientSocket, uint32_t events) {
    // 检查错误或断开连接事件
    if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
        LOG_INFO("Client disconnected or error on fd: " + std::to_string(clientSocket));
        cleanupClient(clientSocket);
        return;
    }

    // 获取客户端信息
    ClientInfo clientInfo;
    {
        std::lock_guard<std::mutex> lock(m_clientInfoMutex);
        auto it = m_clientInfoMap.find(clientSocket);
        if (it == m_clientInfoMap.end()) {
            LOG_WARN("Client info not found for fd: " + std::to_string(clientSocket));
            cleanupClient(clientSocket);
            return;
        }
        // 取出客户端信息后从map中移除（表示正在处理中）
        clientInfo = it->second;
        m_clientInfoMap.erase(it);
    }

    // 从epoll中移除该fd（避免重复触发）
    m_epollManager->removeFd(clientSocket);

    // 将请求处理提交到线程池
    m_threadPool->enqueue([this, clientSocket, clientInfo]() {
        // 在线程池中处理请求
        this->handleClient(clientSocket, clientInfo.ip, clientInfo.port);
        // 处理完成后关闭socket
        close(clientSocket);
    });
}

/**
 * @brief 清理客户端连接资源
 *
 * 从epoll中移除、关闭socket、移除客户端信息
 *
 * @param clientSocket 客户端socket描述符
 */
void HttpServer::cleanupClient(int clientSocket) {
    // 从epoll中移除
    if (m_epollManager) {
        m_epollManager->removeFd(clientSocket);
    }
    // 关闭socket
    close(clientSocket);
    // 移除客户端信息
    {
        std::lock_guard<std::mutex> lock(m_clientInfoMutex);
        m_clientInfoMap.erase(clientSocket);
    }
}

/**
 * @brief 处理客户端请求
 *
 * 完整的请求处理流程：
 * 1. 解析HTTP请求
 * 2. 调用处理函数生成响应
 * 3. 发送响应头
 * 4. 发送响应体（文件或内存内容）
 * 5. 关闭客户端连接
 *
 * @param clientSocket 客户端socket描述符
 * @param clientIp 客户端IP地址
 * @param clientPort 客户端端口号
 */
void HttpServer::handleClient(int clientSocket, const std::string& clientIp, int clientPort) {
    // 记录请求开始时间
    auto startTime = std::chrono::steady_clock::now();

    // 初始化请求和响应对象
    HttpRequest request;
    request.setClientIp(clientIp);
    request.setClientPort(clientPort);

    HttpResponse response;

    try {
        // 步骤1：解析HTTP请求
        request = parseRequest(clientSocket);

        // 检查请求是否有效（如果解析失败，request会是默认构造的无效对象）
        if (request.getMethod() == HttpRequest::METHOD_UNKNOWN || request.getUrl().empty()) {
            response = HttpResponse::badRequest();
            LOG_WARN("Invalid request from " + clientIp + ":" + std::to_string(clientPort));
        } else {
            // 步骤2：根据请求类型调用相应处理函数
            if (m_requestHandler) {
                // 使用自定义处理函数
                response = m_requestHandler(request);
            } else {
                // 默认处理
                if (request.getMethod() == HttpRequest::METHOD_GET) {
                    // 处理API端点
                    if (request.getPath() == "/api/echo") {
                        response = handleApiEcho(request);
                    } else {
                        // GET请求提供静态文件服务
                        response = handleStaticFile(request);
                    }
                } else if (request.getMethod() == HttpRequest::METHOD_HEAD) {
                    // HEAD请求处理：与GET相同，但不返回响应体
                    if (request.getPath() == "/api/echo") {
                        response = handleApiEcho(request);
                    } else {
                        response = handleStaticFile(request);
                    }
                    // HEAD请求不返回响应体，清空body和文件路径
                    // 注意：保留Content-Type，因为HEAD响应应该包含与GET相同的头部信息
                    std::string contentType = response.getContentType();
                    response.setBody("");
                    response.setFilePath("");
                    response.setContentType(contentType);
                } else if (request.getMethod() == HttpRequest::METHOD_POST) {
                    // POST请求处理
                    response = handlePostRequest(request);
                } else {
                    // 其他HTTP方法返回501 Not Implemented
                    response = HttpResponse::notImplemented();
                }
            }
        }
    } catch (const std::exception& e) {
        // 捕获异常并返回500错误
        LOG_ERROR("Request handling exception: " + std::string(e.what()));
        response = HttpResponse::internalServerError();
    }

    // 步骤4：发送响应体
    size_t responseSize = 0;
    std::string bodyToSend;
    bool useCache = false;
    bool useSendfile = false;  // 是否使用sendfile优化
    int fileFd = -1;           // 文件描述符（用于sendfile）
    off_t fileOffset = 0;      // 文件偏移量
    size_t fileSize = 0;       // 文件大小

    if (!response.getFilePath().empty() && response.getStatusCode() == HttpResponse::STATUS_200_OK) {
        // 优先尝试从缓存获取文件内容
        if (m_fileCache && m_fileCache->isEnabled()) {
            auto cachedContent = m_fileCache->get(response.getFilePath());
            if (cachedContent) {
                // 缓存命中，使用缓存内容
                bodyToSend = *cachedContent;
                responseSize = cachedContent->size();
                useCache = true;
            }
        }

        if (!useCache) {
            // 缓存未命中，使用sendfile零拷贝优化发送文件
            // sendfile直接在内核空间将文件内容发送到socket，避免用户空间拷贝
            fileFd = open(response.getFilePath().c_str(), O_RDONLY);
            if (fileFd >= 0) {
                struct stat fileStat;
                if (fstat(fileFd, &fileStat) == 0) {
                    fileSize = fileStat.st_size;
                    responseSize = fileSize;
                    useSendfile = true;
                    
                    // 如果文件较小，仍然使用缓存
                    if (m_fileCache && m_fileCache->isEnabled() && fileSize <= m_fileCache->getMaxFileSize()) {
                        char buffer[8192];
                        ssize_t bytesRead;
                        std::string fileContent;
                        lseek(fileFd, 0, SEEK_SET);  // 重置文件偏移
                        while ((bytesRead = read(fileFd, buffer, sizeof(buffer))) > 0) {
                            fileContent.append(buffer, bytesRead);
                        }
                        m_fileCache->put(response.getFilePath(), fileContent, fileStat.st_mtime);
                        lseek(fileFd, 0, SEEK_SET);  // 重置文件偏移供sendfile使用
                    }
                }
            }
        }
    } else if (!response.getBody().empty()) {
        bodyToSend = response.getBody();
        responseSize = bodyToSend.size();
    }

    // 步骤3：发送HTTP响应
    std::string headerStr = response.buildHeaderString(responseSize);
    sendData(clientSocket, headerStr.c_str(), headerStr.size());
    
    if (useSendfile && fileFd >= 0) {
        // 使用sendfile零拷贝发送文件内容，性能更优
        ssize_t sent;
        while (fileSize > 0) {
            sent = sendfile(clientSocket, fileFd, &fileOffset, fileSize);
            if (sent <= 0) {
                if (errno == EINTR) {
                    continue;  // 被信号中断，重试
                }
                break;  // 发送错误或连接关闭
            }
            fileSize -= sent;
        }
        close(fileFd);
    } else if (!bodyToSend.empty()) {
        // 使用缓存内容或普通响应体发送
        sendData(clientSocket, bodyToSend.c_str(), bodyToSend.size());
    }

    // 步骤5：关闭客户端连接
    close(clientSocket);

    // 计算处理耗时
    auto endTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
    double durationMs = duration.count() / 1000.0;

    // 记录访问日志
    std::string method = HttpRequest::methodToString(request.getMethod());
    std::string url = request.getUrl();
    int statusCode = response.getStatusCode();

    LOG_ACCESS(clientIp, method, url, statusCode, responseSize, durationMs);
}

/**
 * @brief 解析HTTP请求
 * 
 * 从socket读取并解析HTTP请求行和头部，
 * 构造HttpRequest对象。
 * 
 * HTTP请求格式：
 * @code
 * GET /path HTTP/1.1\r\n
 * Host: example.com\r\n
 * Content-Type: text/html\r\n
 * \r\n
 * [body]
 * @endcode
 * 
 * @param clientSocket 客户端socket
 * @return HttpRequest 解析后的请求对象
 */
HttpRequest HttpServer::parseRequest(int clientSocket) const {
    HttpRequest request;
    std::string line;

    // 读取请求行（第一行）
    if (readLine(clientSocket, line) <= 0) {
        // 如果请求行读取失败，返回空的请求对象
        // 这会导致后续处理返回400错误，而不是默认构造的无效请求
        return HttpRequest();
    }

    // 检查请求行是否有效（至少包含方法和URL）
    if (line.empty()) {
        return HttpRequest();
    }

    // 解析请求行：METHOD URL HTTP/VERSION
    std::istringstream requestLine(line);
    std::string method, url, version;
    requestLine >> method >> url >> version;

    // 检查请求行是否完整
    if (method.empty() || url.empty() || version.empty()) {
        return HttpRequest();
    }

    // 设置请求方法和URL
    request.setMethodString(method);
    request.setUrl(url);

    // 读取HTTP头部
    while (readLine(clientSocket, line) > 0) {
        // 空行表示头部结束
        if (line.empty()) {
            break;
        }

        // 解析头部字段：Key: Value
        size_t colonPos = line.find(':');
        if (colonPos != std::string::npos) {
            std::string key = line.substr(0, colonPos);
            std::string value = line.substr(colonPos + 1);

            // 去除首尾空白
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            request.addHeader(key, value);
        }
    }

    // 读取请求体（如果存在）
    std::string contentLengthStr = request.getHeader("Content-Length");
    if (!contentLengthStr.empty()) {
        // 解析Content-Length获取请求体大小
        size_t contentLength = std::stoul(contentLengthStr);
        
        // 限制最大请求体大小为10MB，防止DoS攻击
        if (contentLength > 0 && contentLength <= 10 * 1024 * 1024) {
            std::string body;
            body.resize(contentLength);
            readData(clientSocket, &body[0], contentLength);
            request.setBody(body);
        }
    }

    return request;
}

/**
 * @brief 处理静态文件请求
 * 
 * 根据请求路径查找对应的文件，并生成HTTP响应。
 * 支持：
 * - 文件直接返回
 * - 目录自动查找index.html
 * - 目录列表（当无index.html时）
 * 
 * @param request 客户端请求对象
 * @return HttpResponse 文件响应
 */
HttpResponse HttpServer::handleStaticFile(const HttpRequest& request) const {
    // 获取请求路径
    std::string urlPath = request.getPath();

    // 安全检查：防止路径遍历攻击
    if (isPathTraversal(urlPath)) {
        return HttpResponse::badRequest();
    }

    // URL解码
    std::string decodedPath = urlDecode(urlPath);
    
    // 再次检查解码后的路径
    if (isPathTraversal(decodedPath)) {
        return HttpResponse::badRequest();
    }

    // 构造完整文件路径
    std::string filePath = m_docRoot + decodedPath;

    // 获取文件状态
    struct stat st;
    if (stat(filePath.c_str(), &st) < 0) {
        // 文件不存在
        LOG_WARN("File not found: " + filePath);
        return HttpResponse::notFound();
    }

    // 如果请求的是目录
    if (S_ISDIR(st.st_mode)) {
        // 优先查找index.html
        std::string indexPath = filePath + "/index.html";
        if (stat(indexPath.c_str(), &st) == 0) {
            filePath = indexPath;
        } else {
            // 没有index.html，生成目录列表
            return handleDirectory(filePath);
        }
    }

    // 创建成功响应
    HttpResponse response;
    response.setStatusCode(HttpResponse::STATUS_200_OK);
    response.setFilePath(filePath);  // 设置文件路径，响应类会自动识别Content-Type

    return response;
}

/**
 * @brief 处理目录请求 - 生成目录列表
 * 
 * 当目录中没有index.html时，生成一个HTML页面
 * 列出目录中的所有文件和子目录。
 * 
 * @param dirPath 目录路径
 * @return HttpResponse 包含HTML目录列表的响应
 */
HttpResponse HttpServer::handleDirectory(const std::string& dirPath) const {
    HttpResponse response;
    response.setStatusCode(HttpResponse::STATUS_200_OK);
    response.setContentType("text/html; charset=utf-8");

    // 生成HTML头部
    std::string html = "<html><head><title>Directory Listing</title></head><body>";
    
    // 显示目录标题（相对路径）
    html += "<h1>Directory: " + dirPath.substr(m_docRoot.length()) + "</h1>";
    html += "<ul>";

    // 打开目录
    DIR* dir = opendir(dirPath.c_str());
    if (dir) {
        struct dirent* entry;
        
        // 遍历目录中的所有条目
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            
            // 跳过"."（当前目录）和".."（父目录）
            if (name == "." || name == "..") continue;

            std::string fullPath = dirPath + "/" + name;
            struct stat st;
            
            if (stat(fullPath.c_str(), &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    // 目录：添加"/"后缀
                    html += "<li><a href=\"" + name + "/\">" + name + "/</a></li>";
                } else {
                    // 文件：直接链接
                    html += "<li><a href=\"" + name + "\">" + name + "</a></li>";
                }
            }
        }
        closedir(dir);
    }

    html += "</ul></body></html>";
    response.setBody(html);

    return response;
}

/**
 * @brief URL解码
 * 
 * 将URL编码的字符串转换为普通字符串。
 * 处理以下情况：
 * - %XX: 十六进制编码的字符
 * - +:   空格（HTTP表单中的特殊处理）
 * 
 * @param path URL编码的路径
 * @return std::string 解码后的字符串
 */
std::string HttpServer::urlDecode(const std::string& path) const {
    std::string result;
    
    for (size_t i = 0; i < path.length(); ++i) {
        if (path[i] == '%' && i + 2 < path.length()) {
            // %XX 格式：两位十六进制数
            int value;
            std::istringstream iss(path.substr(i + 1, 2));
            if (iss >> std::hex >> value) {
                result += static_cast<char>(value);
                i += 2;  // 跳过已处理的两个十六进制字符
            } else {
                // 无效的%编码，原样保留
                result += path[i];
            }
        } else if (path[i] == '+') {
            // + 号代表空格（application/x-www-form-urlencoded格式）
            result += ' ';
        } else {
            // 普通字符
            result += path[i];
        }
    }
    
    return result;
}

/**
 * @brief 路径规范化
 * 
 * 移除路径中的冗余部分：
 * - "/./": 当前目录标记
 * - "//": 连续斜杠
 * 
 * @param path 原始路径
 * @return std::string 规范化后的路径
 */
std::string HttpServer::normalizePath(const std::string& path) const {
    std::string result = path;

    // 移除 "/./"
    size_t pos;
    while ((pos = result.find("/./")) != std::string::npos) {
        result.erase(pos, 2);
    }

    // 移除 "//"
    while ((pos = result.find("//")) != std::string::npos) {
        result.erase(pos, 1);
    }

    return result;
}

/**
 * @brief 检查路径遍历攻击
 * 
 * 检测路径中是否包含".."（父目录引用），
 * 这可能允许攻击者访问文档根目录之外的文件。
 * 
 * @param path 待检测的路径
 * @return bool 存在风险返回true，安全返回false
 */
bool HttpServer::isPathTraversal(const std::string& path) const {
    // 先规范化路径
    std::string normalized = normalizePath(path);
    
    // 检查是否包含".."
    return normalized.find("..") != std::string::npos;
}

/**
 * @brief 从socket读取一行
 *
 * 读取直到遇到换行符（\\n）的数据。
 * 处理不同换行符格式：\\n, \\r\\n, \\r
 *
 * 在epoll边缘触发(ET)模式下，必须使用非阻塞IO并处理EAGAIN错误。
 *
 * @param socket socket描述符
 * @param line 存储读取结果的字符串
 * @return int 读取的字节数，-1表示错误或连接关闭
 */
int HttpServer::readLine(int socket, std::string& line) const {
    line.clear();
    char ch;
    ssize_t n;

    // 按字节读取
    while (true) {
        n = read(socket, &ch, 1);
        if (n < 0) {
            // 非阻塞模式下，EAGAIN表示数据已读完
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 如果已经读到了一些数据，返回成功
                // 否则返回-1表示需要等待更多数据
                return line.empty() ? -1 : line.length();
            }
            // 其他错误
            if (line.empty()) {
                return -1;
            }
            return line.length();
        }
        if (n == 0) {
            // 连接关闭
            return line.empty() ? -1 : line.length();
        }

        if (ch == '\n') {
            // 换行符，行的结束
            break;
        }

        // 跳过\r（处理\r\n和\r的情况）
        if (ch != '\r') {
            line += ch;
        }
    }

    return line.length();
}

/**
 * @brief 从socket读取指定数量的数据
 *
 * 确保读取到指定数量的字节（除非遇到EOF或错误）。
 *
 * 在epoll边缘触发(ET)模式下，必须使用非阻塞IO并处理EAGAIN错误。
 *
 * @param socket socket描述符
 * @param buffer 数据缓冲区
 * @param size 要读取的字节数
 * @return ssize_t 实际读取的字节数
 */
ssize_t HttpServer::readData(int socket, char* buffer, size_t size) const {
    size_t totalRead = 0;
    ssize_t n;

    // 循环读取直到达到指定数量
    while (totalRead < size) {
        n = read(socket, buffer + totalRead, size - totalRead);
        if (n < 0) {
            // 非阻塞模式下，EAGAIN表示数据已读完
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            // 其他错误
            break;
        }
        if (n == 0) {
            // EOF，连接关闭
            break;
        }
        totalRead += n;
    }

    return totalRead;
}

/**
 * @brief 向socket发送数据
 * 
 * 确保发送完所有数据（除非遇到错误）。
 * 处理中断信号（EINTR）导致的write中断。
 * 
 * @param socket socket描述符
 * @param data 要发送的数据指针
 * @param size 要发送的字节数
 * @return ssize_t 实际发送的字节数
 */
ssize_t HttpServer::sendData(int socket, const char* data, size_t size) const {
    size_t totalSent = 0;
    ssize_t n;

    // 循环发送直到全部发送完成
    while (totalSent < size) {
        n = write(socket, data + totalSent, size - totalSent);
        
        if (n <= 0) {
            // 被中断信号打断，继续尝试
            if (errno == EINTR) {
                continue;
            }
            // 发送错误或连接关闭
            break;
        }
        
        totalSent += n;
    }

    return totalSent;
}

/**
 * @brief 处理POST请求
 * 
 * 处理POST请求，支持以下功能：
 * - 表单数据解析（application/x-www-form-urlencoded）
 * - API端点处理（/api/echo）
 * - 返回JSON格式的响应
 * 
 * @param request 客户端POST请求对象
 * @return HttpResponse POST响应对象
 */
HttpResponse HttpServer::handlePostRequest(const HttpRequest& request) const {
    HttpResponse response;
    response.setStatusCode(HttpResponse::STATUS_200_OK);
    response.addHeader("Content-Type", "application/json; charset=utf-8");

    std::string path = request.getPath();
    
    // 处理 /api/echo 端点 - 用于测试POST请求
    if (path == "/api/echo") {
        // 构建JSON响应
        std::ostringstream json;
        json << "{\n";
        json << "  \"method\": \"POST\",\n";
        json << "  \"path\": \"" << path << "\",\n";
        json << "  \"contentType\": \"" << request.getContentType() << "\",\n";
        json << "  \"body\": \"" << request.getBody() << "\",\n";
        
        // 解析并输出表单数据
        auto formData = request.parseFormData();
        json << "  \"formData\": {\n";
        bool first = true;
        for (const auto& pair : formData) {
            if (!first) json << ",\n";
            json << "    \"" << pair.first << "\": \"" << pair.second << "\"";
            first = false;
        }
        json << "\n  }\n";
        json << "}";
        
        response.setBody(json.str());
        return response;
    }
    
    // 其他POST请求返回简单的确认信息
    std::ostringstream json;
    json << "{\n";
    json << "  \"status\": \"success\",\n";
    json << "  \"message\": \"POST request received\",\n";
    json << "  \"path\": \"" << path << "\",\n";
    json << "  \"bodyLength\": " << request.getBody().length() << "\n";
    json << "}";
    
    response.setBody(json.str());
    return response;
}

/**
 * @brief 处理API Echo端点
 *
 * 处理 /api/echo 请求，返回请求信息（用于测试）。
 * 支持GET和POST请求，返回解析后的查询参数或表单数据。
 *
 * @param request 客户端请求对象
 * @return HttpResponse JSON响应对象
 */
HttpResponse HttpServer::handleApiEcho(const HttpRequest& request) const {
    HttpResponse response;
    response.setStatusCode(HttpResponse::STATUS_200_OK);
    response.addHeader("Content-Type", "application/json; charset=utf-8");

    std::string path = request.getPath();
    std::string method = HttpRequest::methodToString(request.getMethod());

    // 构建JSON响应
    std::ostringstream json;
    json << "{\n";
    json << "  \"method\": \"" << method << "\",\n";
    json << "  \"path\": \"" << path << "\",\n";
    json << "  \"url\": \"" << request.getUrl() << "\",\n";

    // 解析并输出查询参数
    auto queryParams = request.parseQueryParams();
    json << "  \"queryParams\": {\n";
    bool first = true;
    for (const auto& pair : queryParams) {
        if (!first) json << ",\n";
        json << "    \"" << pair.first << "\": \"" << pair.second << "\"";
        first = false;
    }
    json << "\n  }";

    // 如果是POST请求，也输出表单数据
    if (request.getMethod() == HttpRequest::METHOD_POST) {
        json << ",\n";
        json << "  \"contentType\": \"" << request.getContentType() << "\",\n";
        json << "  \"body\": \"" << request.getBody() << "\",\n";

        auto formData = request.parseFormData();
        json << "  \"formData\": {\n";
        first = true;
        for (const auto& pair : formData) {
            if (!first) json << ",\n";
            json << "    \"" << pair.first << "\": \"" << pair.second << "\"";
            first = false;
        }
        json << "\n  }";
    }

    json << "\n}";

    response.setBody(json.str());
    return response;
}
