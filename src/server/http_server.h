/**
 * @file http_server.h
 * @brief HTTP服务器类的头文件定义
 * 
 * 本文件定义了HttpServer类，提供一个支持多线程的HTTP服务器实现。
 * 服务器支持静态文件服务、目录列表、自定义请求处理等功能。
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

// C++标准库头文件
#include <string>          // 字符串处理
#include <memory>          // 智能指针
#include <functional>      // 函数对象
#include <thread>          // 线程支持
#include <atomic>          // 原子操作
#include <unordered_map>   // 哈希映射
#include <mutex>           // 互斥锁

// 前向声明 - 避免循环依赖
class ThreadPool;
class HttpRequest;
class HttpResponse;
class EpollManager;  // epoll事件管理器前向声明
class FileCache;     // 文件缓存前向声明

// 包含日志系统头文件
#include "../logger/logger.h"

// 包含epoll管理器头文件
#include "epoll_manager.h"

/**
 * @brief HTTP服务器类
 * 
 * 一个支持多线程的HTTP服务器实现，提供以下功能：
 * - TCP监听和客户端连接接受
 * - HTTP请求解析
 * - 静态文件服务
 * - 目录列表自动生成
 * - 路径遍历攻击防护
 * - 可自定义请求处理
 * - 优雅关闭支持
 * 
 * 使用示例：
 * @code
 * HttpServer server("0.0.0.0", 8080);
 * server.setDocRoot("/var/www/html");
 * server.setNumThreads(4);
 * server.start();
 * @endcode
 */
class HttpServer {
public:
    /**
     * @brief 请求处理函数类型定义
     * 
     * 用户可以通过设置自定义请求处理函数来扩展服务器功能。
     * 处理函数接收HttpRequest对象，返回HttpResponse对象。
     * 
     * @param request 客户端HTTP请求对象
     * @return HttpResponse 服务器响应对象
     */
    using RequestHandler = std::function<HttpResponse(const HttpRequest& request)>;

    /**
     * @brief 构造函数
     * 
     * 创建HTTP服务器实例，初始化默认配置。
     * 
     * @param ip 服务器绑定的IP地址（默认"0.0.0.0"表示监听所有接口）
     * @param port 服务器监听的端口号（默认8080）
     */
    HttpServer(const std::string& ip = "0.0.0.0", int port = 8080);

    /**
     * @brief 析构函数
     * 
     * 确保服务器正确停止并释放所有资源。
     */
    ~HttpServer();

    /**
     * @brief 启动HTTP服务器
     * 
     * 初始化服务器socket，绑定地址和端口，开始监听连接请求。
     * 如果服务器已经在运行，则返回false。
     * 
     * @return bool 启动成功返回true，否则返回false
     * 
     * @note 启动后会创建线程池来处理客户端请求
     */
    bool start();

    /**
     * @brief 停止HTTP服务器
     * 
     * 优雅停止服务器：
     * 1. 设置停止标志
     * 2. 关闭服务器socket
     * 3. 关闭线程池
     * 
     * 已连接的客户端将继续完成其请求。
     */
    void stop();

    /**
     * @brief 检查服务器是否正在运行
     * 
     * @return bool 服务器运行返回true，否则返回false
     */
    bool isRunning() const;

    /**
     * @brief 设置服务器端口
     * 
     * @param port 端口号（有效范围：1-65535）
     * 
     * @note 此方法应在start()之前调用
     */
    void setPort(int port);

    /**
     * @brief 获取服务器端口
     * 
     * @return int 当前配置的端口号
     */
    int getPort() const;

    /**
     * @brief 设置文档根目录
     * 
     * 指定服务器提供静态文件的根目录。
     * 客户端请求的文件将相对于此目录进行解析。
     * 
     * @param docRoot 文档根目录路径
     * 
     * @note 此方法应在start()之前调用
     */
    void setDocRoot(const std::string& docRoot);

    /**
     * @brief 获取文档根目录
     * 
     * @return std::string 当前配置的文档根目录
     */
    std::string getDocRoot() const;

    /**
     * @brief 设置线程池大小
     * 
     * 指定处理客户端请求的工作线程数量。
     * 
     * @param numThreads 线程数量（建议4-32之间）
     * 
     * @note 此方法应在start()之前调用
     * @note 过多的线程会增加上下文切换开销
     */
    void setNumThreads(int numThreads);

    /**
     * @brief 获取线程池大小
     * 
     * @return int 当前配置的线程数量
     */
    int getNumThreads() const;

    /**
     * @brief 启用或禁用静态文件缓存
     * 
     * @param enabled true启用缓存，false禁用缓存
     */
    void setCacheEnabled(bool enabled);

    /**
     * @brief 检查缓存是否启用
     * 
     * @return bool 缓存启用返回true，否则返回false
     */
    bool isCacheEnabled() const;

    /**
     * @brief 设置最大缓存大小
     * 
     * @param maxSize 最大缓存大小（字节）
     */
    void setCacheMaxSize(size_t maxSize);

    /**
     * @brief 获取最大缓存大小
     * 
     * @return size_t 最大缓存大小（字节）
     */
    size_t getCacheMaxSize() const;

    /**
     * @brief 设置单文件最大缓存大小
     * 
     * @param maxSize 单文件最大缓存大小（字节）
     */
    void setCacheMaxFileSize(size_t maxSize);

    /**
     * @brief 获取单文件最大缓存大小
     * 
     * @return size_t 单文件最大缓存大小（字节）
     */
    size_t getCacheMaxFileSize() const;

    /**
     * @brief 获取缓存统计信息
     * 
     * @return std::string 缓存统计信息的JSON格式字符串
     */
    std::string getCacheStats() const;

    /**
     * @brief 清空文件缓存
     */
    void clearCache();

    /**
     * @brief 设置自定义请求处理函数
     * 
     * 设置用户定义的请求处理函数，用于处理HTTP请求。
     * 如果未设置自定义处理函数，服务器将使用默认的静态文件服务。
     * 
     * @param handler RequestHandler类型的可调用对象
     * 
     * @note 自定义处理函数可以返回自定义的HttpResponse
     * 
     * @par 示例：
     * @code
     * server.setRequestHandler([](const HttpRequest& request) {
     *     HttpResponse response;
     *     response.setStatusCode(HttpResponse::STATUS_200_OK);
     *     response.setBody("Hello, World!");
     *     return response;
     * });
     * @endcode
     */
    void setRequestHandler(RequestHandler handler);

    /**
     * @brief 获取本地IP地址
     * 
     * 获取服务器绑定的实际IP地址。
     * 
     * @return std::string 服务器的IP地址字符串
     */
    std::string getLocalIp() const;

private:
    /**
     * @brief 接受客户端连接（传统线程池模式）
     *
     * 在独立线程中运行，持续接受新的客户端连接。
     * 每个新连接会被加入到线程池的任务队列中。
     *
     * @note 此方法在start()中启动的独立线程中运行
     * @note 当useEpoll_为true时，此方法不使用
     */
    void acceptConnections();

    /**
     * @brief 使用epoll接受客户端连接
     *
     * 在独立线程中运行，使用epoll_wait等待连接事件。
     * 新连接被添加到epoll监听集合中。
     *
     * @note 此方法在start()中启动的独立线程中运行（epoll模式）
     */
    void acceptConnectionsEpoll();

    /**
     * @brief 处理epoll事件循环
     *
     * 在独立线程中运行，调用epoll_wait等待并处理IO事件。
     *
     * @note 此方法在start()中启动的独立线程中运行（epoll模式）
     */
    void epollEventLoop();

    /**
     * @brief 处理客户端可读事件（epoll模式）
     *
     * 当epoll检测到客户端socket可读时调用。
     * 在边缘触发(ET)模式下，必须循环读取直到EAGAIN，确保读完所有数据。
     *
     * @param clientSocket 客户端socket描述符
     * @param events epoll事件标志
     */
    void handleClientRead(int clientSocket, uint32_t events);

    /**
     * @brief 处理服务器socket可读事件（epoll模式）
     *
     * 当epoll检测到服务器socket可读时调用（有新连接到来）。
     *
     * @param serverSocket 服务器socket描述符
     * @param events epoll事件标志
     */
    void handleServerRead(int serverSocket, uint32_t events);

    /**
     * @brief 清理客户端连接资源
     *
     * 从epoll中移除、关闭socket、移除客户端信息
     *
     * @param clientSocket 客户端socket描述符
     */
    void cleanupClient(int clientSocket);

    /**
     * @brief 处理客户端请求
     *
     * 解析HTTP请求，调用处理函数生成响应，并发送回客户端。
     * 支持HTTP Keep-Alive，可在一个连接上处理多个请求。
     *
     * @param clientSocket 客户端连接的socket描述符
     * @param clientIp 客户端IP地址
     * @param clientPort 客户端端口号
     * @return bool 返回true表示连接应保持（Keep-Alive），false表示应关闭连接
     */
    bool handleClient(int clientSocket, const std::string& clientIp, int clientPort);

    /**
     * @brief 解析HTTP请求
     * 
     * 从socket读取数据并解析为HttpRequest对象。
     * 
     * @param clientSocket 客户端socket描述符
     * @return HttpRequest 解析后的请求对象
     */
    HttpRequest parseRequest(int clientSocket) const;

    /**
     * @brief 处理静态文件请求
     * 
     * 根据请求路径查找对应的静态文件并生成响应。
     * 
     * @param request 客户端请求对象
     * @return HttpResponse 文件响应对象
     */
    HttpResponse handleStaticFile(const HttpRequest& request) const;

    /**
     * @brief 处理目录请求
     * 
     * 当请求目标是目录且目录中没有index.html时，
     * 生成目录列表HTML页面。
     * 
     * @param dirPath 目录路径
     * @return HttpResponse 包含目录列表的响应
     */
    HttpResponse handleDirectory(const std::string& dirPath) const;

    /**
     * @brief 处理POST请求
     * 
     * 处理POST请求，支持表单数据解析和API端点。
     * 
     * @param request 客户端POST请求对象
     * @return HttpResponse POST响应对象
     */
    HttpResponse handlePostRequest(const HttpRequest& request) const;

    /**
     * @brief 处理API Echo端点
     * 
     * 处理 /api/echo 请求，返回请求信息（用于测试）。
     * 
     * @param request 客户端请求对象
     * @return HttpResponse JSON响应对象
     */
    HttpResponse handleApiEcho(const HttpRequest& request) const;

    /**
     * @brief URL解码
     * 
     * 将URL编码的字符串转换为普通字符串。
     * 处理%XX转义序列和+号空格。
     * 
     * @param path URL编码的路径
     * @return std::string 解码后的路径
     */
    std::string urlDecode(const std::string& path) const;

    /**
     * @brief 路径规范化
     * 
     * 移除路径中的"./"和"//"等冗余部分。
     * 
     * @param path 原始路径
     * @return std::string 规范化后的路径
     */
    std::string normalizePath(const std::string& path) const;

    /**
     * @brief 检查路径遍历攻击
     * 
     * 检测路径中是否包含".."以防止目录遍历攻击。
     * 
     * @param path 待检测的路径
     * @return bool 如果存在路径遍历风险返回true
     */
    bool isPathTraversal(const std::string& path) const;

    /**
     * @brief 从socket读取一行
     * 
     * 读取直到遇到换行符为止的数据。
     * 
     * @param socket socket描述符
     * @param line 存储读取结果的字符串引用
     * @return int 读取的字节数，-1表示错误或连接关闭
     */
    int readLine(int socket, std::string& line) const;

    /**
     * @brief 从socket读取数据
     * 
     * 读取指定数量的字节数据。
     * 
     * @param socket socket描述符
     * @param buffer 存储数据的缓冲区
     * @param size 要读取的字节数
     * @return ssize_t 实际读取的字节数
     */
    ssize_t readData(int socket, char* buffer, size_t size) const;

    /**
     * @brief 向socket发送数据
     * 
     * 发送指定数量的字节数据。
     * 
     * @param socket socket描述符
     * @param data 要发送的数据指针
     * @param size 要发送的字节数
     * @return ssize_t 实际发送的字节数
     */
    ssize_t sendData(int socket, const char* data, size_t size) const;

    //================== 成员变量 ==================

    /** 服务器绑定的IP地址 */
    std::string m_ip;

    /** 服务器监听端口 */
    int m_port;

    /** 文档根目录路径 */
    std::string m_docRoot;

    /** 线程池工作线程数量 */
    int m_numThreads;

    /** 服务器监听socket描述符 */
    int m_serverSocket;

    /** 服务器运行状态标志（原子操作保证线程安全） */
    std::atomic<bool> m_running;

    /** 线程池智能指针 */
    std::unique_ptr<ThreadPool> m_threadPool;

    /** 接受客户端连接的线程 */
    std::thread m_acceptThread;

    /** 自定义请求处理函数 */
    RequestHandler m_requestHandler;

    //================== epoll相关成员变量 ==================

    /** 是否使用epoll模式 */
    bool m_useEpoll;

    /** epoll事件管理器 */
    std::unique_ptr<EpollManager> m_epollManager;

    /** epoll事件处理线程 */
    std::thread m_epollThread;

    /** 客户端信息结构体（用于epoll模式） */
    struct ClientInfo {
        std::string ip;     // 客户端IP地址
        int port;           // 客户端端口号
        std::string readBuffer;  // 读取缓冲区（用于边缘触发模式）
    };

    /** 客户端信息映射（fd -> ClientInfo） */
    std::unordered_map<int, ClientInfo> m_clientInfoMap;

    /** 客户端信息映射的互斥锁 */
    std::mutex m_clientInfoMutex;

    //================== 文件缓存相关成员变量 ==================

    /** 文件缓存智能指针 */
    std::unique_ptr<FileCache> m_fileCache;
};

#endif // HTTP_SERVER_H
