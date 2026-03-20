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

// 前向声明 - 避免循环依赖
class ThreadPool;
class HttpRequest;
class HttpResponse;

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
     * @brief 接受客户端连接
     * 
     * 在独立线程中运行，持续接受新的客户端连接。
     * 每个新连接会被加入到线程池的任务队列中。
     * 
     * @note 此方法在start()中启动的独立线程中运行
     */
    void acceptConnections();

    /**
     * @brief 处理客户端请求
     * 
     * 解析HTTP请求，调用处理函数生成响应，并发送回客户端。
     * 
     * @param clientSocket 客户端连接的socket描述符
     * @param clientIp 客户端IP地址
     * @param clientPort 客户端端口号
     */
    void handleClient(int clientSocket, const std::string& clientIp, int clientPort);

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

    /** 自定义请求处理函数 */
    RequestHandler m_requestHandler;
};

#endif // HTTP_SERVER_H
