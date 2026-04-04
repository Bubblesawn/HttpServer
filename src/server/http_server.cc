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
#include "../server/tcp_server.h"
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
#include <iomanip>           // 日期格式化
#include <ctime>             // 时间处理
#include <algorithm>         // 算法
#include <cctype>           // 字符处理
#include <limits>           // 数值边界
#include <iostream>          // 输入输出
#include <unordered_map>     // socket读取缓冲
#include <vector>

#include <openssl/err.h>

#include <nlohmann/json.hpp>

// 为兼容旧版本系统，定义EPOLLRDHUP（如果未定义）
#ifndef EPOLLRDHUP
#define EPOLLRDHUP 0x2000
#endif

namespace {
/**
 * @brief 保护按 socket 复用的跨请求读取缓冲区。
 *
 * 该互斥量用于同步访问 g_socketReadBuffers，避免在多线程处理并发连接时
 * 出现同一文件描述符的读缓冲被并发修改。
 */
std::mutex g_readBufferMutex;

/**
 * @brief 按 socket 保存的增量读取缓冲区。
 *
 * 当 HTTP 请求在边缘触发模式下被分段读取时，服务器会把未消费完的字节
 * 保存在这里，下一次读取同一连接时继续拼接解析。
 */
std::unordered_map<int, std::string> g_socketReadBuffers;

/**
 * @brief 初始化 OpenSSL 全局状态，仅执行一次。
 */
std::once_flag g_openSslInitOnce;

void initializeOpenSsl() {
    std::call_once(g_openSslInitOnce, []() {
        OPENSSL_init_ssl(0, nullptr);
    });
}

/**
 * @brief 获取 OpenSSL 错误栈的字符串表示。
 */
std::string getOpenSslErrorMessage() {
    unsigned long errorCode = ERR_get_error();
    if (errorCode == 0) {
        return "unknown OpenSSL error";
    }

    char buffer[256];
    ERR_error_string_n(errorCode, buffer, sizeof(buffer));
    return buffer;
}

/**
 * @brief 生成输入字符串的小写副本。
 *
 * 该工具函数主要用于对 HTTP 头部名和头部值做大小写不敏感判断，避免修改
 * 原始字符串。
 *
 * @param input 原始字符串
 * @return std::string 小写化后的拷贝
 */
std::string toLowerCopy(const std::string& input) {
    std::string output = input;
    std::transform(output.begin(), output.end(), output.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return output;
}

/**
 * @brief 将 URL 路径按 '/' 分割为段列表。
 *
 * 该函数会忽略连续斜杠和空段，适合用于路由模式匹配和路径规范化判断。
 *
 * @param path 原始路径
 * @return std::vector<std::string> 分段后的路径组件
 */
std::vector<std::string> splitPathSegments(const std::string& path) {
    std::vector<std::string> segments;
    size_t start = 0;

    while (start < path.length()) {
        while (start < path.length() && path[start] == '/') {
            ++start;
        }

        size_t end = start;
        while (end < path.length() && path[end] != '/') {
            ++end;
        }

        if (end > start) {
            segments.emplace_back(path.substr(start, end - start));
        }

        start = end + 1;
    }

    return segments;
}

/**
 * @brief 判断请求是否倾向于 JSON 响应。
 *
 * 判定依据按优先级依次为：
 * - Accept 头包含 application/json
 * - Content-Type 头为 JSON 相关媒体类型
 * - 路径以前缀 /api/ 开头
 *
 * 该逻辑用于统一错误响应的返回格式，保证 API 路径优先返回 JSON。
 *
 * @param request HTTP 请求对象
 * @return bool 如果应返回 JSON 则为 true
 */
bool containsJsonAcceptHeader(const HttpRequest& request) {
    const std::string accept = toLowerCopy(request.getHeader("Accept"));
    if (accept.find("application/json") != std::string::npos) {
        return true;
    }

    const std::string contentType = toLowerCopy(request.getContentType());
    if (contentType.find("application/json") != std::string::npos ||
        contentType.find("+json") != std::string::npos) {
        return true;
    }

    return request.getPath().rfind("/api/", 0) == 0;
}

/**
 * @brief 将字符串值转换为 JSON 值。
 *
 * 如果输入已经是合法 JSON 文本，则按 JSON 结构解析；否则将其作为字符串值
 * 返回，避免把普通文本错误地包装成 JSON 对象。
 *
 * @param value 原始字符串
 * @return nlohmann::json 对应的 JSON 值
 */
nlohmann::json valueToJson(const std::string& value) {
    const auto parsed = nlohmann::json::parse(value, nullptr, false);
    if (!parsed.is_discarded()) {
        return parsed;
    }

    return value;
}

/**
 * @brief 将字符串键值对映射转换为 JSON 对象。
 *
 * 所有值都按字符串原样写入，适合用于查询参数、表单参数和调试型回显。
 *
 * @param values 输入键值对
 * @return nlohmann::json JSON 对象
 */
nlohmann::json mapToJsonObject(const std::map<std::string, std::string>& values) {
    nlohmann::json object = nlohmann::json::object();
    for (const auto& pair : values) {
        object[pair.first] = pair.second;
    }
    return object;
}

/**
 * @brief 将字符串键值对映射转换为 JSON 对象，并尝试解析字段值。
 *
 * 该函数会对每个 value 调用 valueToJson，以便数字、布尔值和嵌套 JSON 能够
 * 以结构化形式保留。
 *
 * @param values 输入键值对
 * @return nlohmann::json JSON 对象
 */
nlohmann::json parsedValueMapToJsonObject(const std::map<std::string, std::string>& values) {
    nlohmann::json object = nlohmann::json::object();
    for (const auto& pair : values) {
        object[pair.first] = valueToJson(pair.second);
    }
    return object;
}

/**
 * @brief 单段 Range 解析结果。
 *
 * NOT_PRESENT 表示请求没有提供 Range 头；
 * VALID 表示 Range 语法正确并可应用；
 * INVALID 表示请求头存在但格式或边界不合法。
 */
enum class RangeParseResult {
    NOT_PRESENT,
    VALID,
    INVALID
};

/**
 * @brief 去除字符串首尾空白字符。
 *
 * 主要用于解析 HTTP 头部值和 Range 片段，确保额外空格不会影响判断。
 *
 * @param input 原始字符串
 * @return std::string 去空白后的结果
 */
std::string trimWhitespace(const std::string& input) {
    const size_t start = input.find_first_not_of(" \t");
    if (start == std::string::npos) {
        return "";
    }
    const size_t end = input.find_last_not_of(" \t");
    return input.substr(start, end - start + 1);
}

/**
 * @brief 解析单段 HTTP Range 请求头。
 *
 * 支持以下语法：
 * - bytes=start-end
 * - bytes=start-
 * - bytes=-suffixLength
 *
 * 该函数只处理单段范围。遇到多段范围、非法前缀或越界值时返回 INVALID；
 * 请求未携带 Range 头时返回 NOT_PRESENT。
 *
 * @param rangeHeader 原始 Range 头值
 * @param fileSize 文件总大小，用于边界裁剪
 * @param rangeStart 解析出的起始字节
 * @param rangeEnd 解析出的结束字节
 * @return RangeParseResult 解析结果
 */
RangeParseResult parseSingleRangeHeader(const std::string& rangeHeader,
                                        off_t fileSize,
                                        off_t& rangeStart,
                                        off_t& rangeEnd) {
    if (rangeHeader.empty()) {
        return RangeParseResult::NOT_PRESENT;
    }

    std::string normalized = trimWhitespace(rangeHeader);
    if (normalized.size() < 6) {
        return RangeParseResult::INVALID;
    }

    std::string prefix = normalized.substr(0, 6);
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (prefix != "bytes=") {
        return RangeParseResult::INVALID;
    }

    std::string spec = trimWhitespace(normalized.substr(6));
    if (spec.empty() || spec.find(',') != std::string::npos) {
        return RangeParseResult::INVALID;
    }

    const size_t dashPos = spec.find('-');
    if (dashPos == std::string::npos) {
        return RangeParseResult::INVALID;
    }

    std::string startPart = trimWhitespace(spec.substr(0, dashPos));
    std::string endPart = trimWhitespace(spec.substr(dashPos + 1));

    if (startPart.empty() && endPart.empty()) {
        return RangeParseResult::INVALID;
    }

    try {
        if (startPart.empty()) {
            long long suffixLen = std::stoll(endPart);
            if (suffixLen <= 0 || fileSize <= 0) {
                return RangeParseResult::INVALID;
            }

            if (suffixLen >= fileSize) {
                rangeStart = 0;
            } else {
                rangeStart = fileSize - suffixLen;
            }
            rangeEnd = fileSize - 1;
            return RangeParseResult::VALID;
        }

        long long parsedStart = std::stoll(startPart);
        if (parsedStart < 0 || fileSize <= 0) {
            return RangeParseResult::INVALID;
        }

        rangeStart = static_cast<off_t>(parsedStart);
        if (rangeStart >= fileSize) {
            return RangeParseResult::INVALID;
        }

        if (endPart.empty()) {
            rangeEnd = fileSize - 1;
            return RangeParseResult::VALID;
        }

        long long parsedEnd = std::stoll(endPart);
        if (parsedEnd < 0) {
            return RangeParseResult::INVALID;
        }

        rangeEnd = static_cast<off_t>(parsedEnd);
        if (rangeEnd < rangeStart) {
            return RangeParseResult::INVALID;
        }
        if (rangeEnd >= fileSize) {
            rangeEnd = fileSize - 1;
        }

        return RangeParseResult::VALID;
    } catch (...) {
        return RangeParseResult::INVALID;
    }
}

/**
 * @brief 删除某个 socket 的残留读取缓冲。
 *
 * 当连接关闭或请求处理完成后调用，避免旧数据在连接重用时污染后续解析。
 *
 * @param fd socket 文件描述符
 */
void clearSocketReadBuffer(int fd) {
    std::lock_guard<std::mutex> lock(g_readBufferMutex);
    g_socketReadBuffers.erase(fd);
}

/**
 * @brief 将响应转换为 HEAD 语义。
 *
 * 保留响应的状态码和头部信息，仅清空正文和文件路径，确保响应头中的
 * Content-Length 与最终发送行为一致。
 *
 * @param response 待调整的响应对象
 */
void stripResponseBodyForHead(HttpResponse& response) {
    const std::string contentType = response.getContentType();
    response.setBody("");
    response.setFilePath("");
    response.setContentType(contentType);
}

/**
 * @brief 将时间戳格式化为 HTTP 日期字符串。
 *
 * 输出格式符合 RFC 9110 的 IMF-fixdate 形式，例如：
 * Mon, 02 Jan 2006 15:04:05 GMT
 *
 * @param value UTC 时间戳
 * @return std::string HTTP 日期字符串
 */
std::string formatHttpDate(time_t value) {
    std::tm gmTime{};
    gmtime_r(&value, &gmTime);

    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::put_time(&gmTime, "%a, %d %b %Y %H:%M:%S GMT");
    return output.str();
}

/**
 * @brief 解析 HTTP 日期字符串。
 *
 * 目前支持与 formatHttpDate 对称的 IMF-fixdate 格式，失败时返回 false。
 *
 * @param value HTTP 日期字符串
 * @param parsedTime 解析后的 UTC 时间戳
 * @return bool 解析成功返回 true
 */
bool parseHttpDate(const std::string& value, time_t& parsedTime) {
    std::tm tm{};
    std::istringstream input(value);
    input.imbue(std::locale::classic());
    input >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");
    if (input.fail()) {
        return false;
    }

    tm.tm_isdst = 0;
    const time_t converted = timegm(&tm);
    if (converted < 0) {
        return false;
    }

    parsedTime = converted;
    return true;
}

/**
 * @brief 去除实体标签的弱标签前缀与包裹引号。
 *
 * 用于比较 If-None-Match 中的 ETag 值时统一格式，兼容 W/"etag"、"etag"
 * 和裸值写法。
 *
 * @param value 原始实体标签值
 * @return std::string 规范化后的标签内容
 */
std::string trimCopy(const std::string& value) {
    const size_t start = value.find_first_not_of(" \t");
    if (start == std::string::npos) {
        return "";
    }

    const size_t end = value.find_last_not_of(" \t");
    return value.substr(start, end - start + 1);
}

std::string normalizeEntityTag(const std::string& value) {
    std::string normalized = trimCopy(value);
    if (normalized.size() >= 2 && (normalized[0] == 'W' || normalized[0] == 'w') && normalized[1] == '/') {
        normalized = trimCopy(normalized.substr(2));
    }

    if (normalized.size() >= 2 && normalized.front() == '"' && normalized.back() == '"') {
        normalized = normalized.substr(1, normalized.size() - 2);
    }

    return normalized;
}

/**
 * @brief 构造静态文件的实体标签。
 *
 * 当前实现基于 inode、文件大小和修改时间生成稳定的弱校验值，用于缓存验证。
 *
 * @param fileStat 文件状态信息
 * @return std::string ETag 字符串
 */
std::string buildStaticFileEtag(const struct stat& fileStat) {
    std::ostringstream output;
    output << '"' << std::hex << static_cast<unsigned long long>(fileStat.st_ino) << '-'
           << static_cast<unsigned long long>(fileStat.st_size) << '-'
           << static_cast<unsigned long long>(fileStat.st_mtime) << '"';
    return output.str();
}

/**
 * @brief 判断 If-None-Match 是否命中当前 ETag。
 *
 * 支持逗号分隔的多个实体标签以及通配符 *。
 *
 * @param headerValue If-None-Match 头部值
 * @param currentEtag 当前资源的 ETag
 * @return bool 如果命中则返回 true
 */
bool matchesIfNoneMatch(const std::string& headerValue, const std::string& currentEtag) {
    const std::string normalizedCurrent = normalizeEntityTag(currentEtag);
    if (normalizedCurrent.empty()) {
        return false;
    }

    const std::string trimmedHeader = trimCopy(headerValue);
    if (trimmedHeader == "*") {
        return true;
    }

    std::istringstream input(headerValue);
    std::string token;
    while (std::getline(input, token, ',')) {
        if (normalizeEntityTag(token) == normalizedCurrent) {
            return true;
        }
    }

    return false;
}

/**
 * @brief 判断请求是否满足缓存未修改条件。
 *
 * 先检查 If-None-Match，再回退到 If-Modified-Since。只要任一条件命中，就可以
 * 返回 304 Not Modified。
 *
 * @param request HTTP 请求对象
 * @param fileStat 文件状态信息
 * @param etag 当前资源的 ETag
 * @return bool 如果可以返回 304 则返回 true
 */
bool isConditionalNotModified(const HttpRequest& request,
                              const struct stat& fileStat,
                              const std::string& etag) {
    const std::string ifNoneMatch = request.getHeader("If-None-Match");
    if (!ifNoneMatch.empty()) {
        return matchesIfNoneMatch(ifNoneMatch, etag);
    }

    const std::string ifModifiedSince = request.getHeader("If-Modified-Since");
    if (!ifModifiedSince.empty()) {
        time_t parsedTime = 0;
        if (parseHttpDate(ifModifiedSince, parsedTime)) {
            return parsedTime >= fileStat.st_mtime;
        }
    }

    return false;
}
} // namespace

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
    , m_running(false)            // 初始状态为未运行
    , m_tcpServer(nullptr)
    , m_requestCounter(0)        // 请求ID计数器初始化为0
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

    registerDefaultRoutes();

    addMiddleware([this](const HttpRequest& request, HttpResponse& response, const std::function<void()>& next) {
        const uint64_t requestId = m_requestCounter.fetch_add(1, std::memory_order_relaxed) + 1;
        response.addHeader("X-Request-Id", std::to_string(requestId));
        next();
        response.addHeader("X-Content-Type-Options", "nosniff");
        if (request.getMethod() == HttpRequest::METHOD_OPTIONS) {
            response.addHeader("Access-Control-Allow-Origin", "*");
        }
    });
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

    if (m_tlsEnabled && !setupTlsContext()) {
        return false;
    }

    m_threadPool = std::make_unique<ThreadPool>(m_numThreads);
    m_fileCache = std::make_unique<FileCache>();
    m_tcpServer = std::make_unique<TcpServer>(m_ip, m_port, m_useEpoll);

    m_tcpServer->setAcceptCallback([this](int clientSocket, const std::string& clientIp, int clientPort) {
        this->handleClientAccepted(clientSocket, clientIp, clientPort);
    });

    if (!m_tcpServer->start()) {
        LOG_ERROR("Failed to start TCP server");
        if (m_threadPool) {
            m_threadPool->shutdown();
            m_threadPool.reset();
        }
        m_fileCache.reset();
        m_tcpServer.reset();
        destroyTlsContext();
        m_running.store(false);
        return false;
    }

    m_running.store(true);

    LOG_INFO("Server started on " + m_ip + ":" + std::to_string(m_port));
    LOG_INFO("Document root: " + m_docRoot);
    LOG_INFO("Thread pool size: " + std::to_string(m_numThreads));
    LOG_INFO(std::string("Transport: ") + (m_tlsEnabled ? "HTTPS" : "HTTP"));
    LOG_INFO(std::string("Mode: ") + (m_useEpoll ? "epoll + thread pool (hybrid)" : "thread pool (one-thread-per-connection)"));

    return true;
}

/**
 * @brief 初始化 TLS 上下文
 */
bool HttpServer::setupTlsContext() {
    if (!m_tlsEnabled) {
        return true;
    }

    initializeOpenSsl();

    destroyTlsContext();

    m_tlsContext = SSL_CTX_new(TLS_server_method());
    if (!m_tlsContext) {
        LOG_ERROR("Failed to create TLS context: " + getOpenSslErrorMessage());
        return false;
    }

    SSL_CTX_set_mode(m_tlsContext, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_min_proto_version(m_tlsContext, TLS1_2_VERSION);
    SSL_CTX_set_options(m_tlsContext, SSL_OP_NO_COMPRESSION);

    if (!m_tlsCipherSuites.empty()) {
        bool cipherConfigured = false;
        if (SSL_CTX_set_cipher_list(m_tlsContext, m_tlsCipherSuites.c_str()) == 1) {
            cipherConfigured = true;
        }

#if OPENSSL_VERSION_NUMBER >= 0x10101000L
        if (SSL_CTX_set_ciphersuites(m_tlsContext, m_tlsCipherSuites.c_str()) == 1) {
            cipherConfigured = true;
        }
#endif

        if (!cipherConfigured) {
            LOG_ERROR("Failed to configure TLS cipher suites: " + m_tlsCipherSuites + " (" + getOpenSslErrorMessage() + ")");
            destroyTlsContext();
            return false;
        }
    }

    if (SSL_CTX_use_certificate_file(m_tlsContext, m_tlsCertFile.c_str(), SSL_FILETYPE_PEM) != 1) {
        LOG_ERROR("Failed to load TLS certificate: " + m_tlsCertFile + " (" + getOpenSslErrorMessage() + ")");
        destroyTlsContext();
        return false;
    }

    if (SSL_CTX_use_PrivateKey_file(m_tlsContext, m_tlsKeyFile.c_str(), SSL_FILETYPE_PEM) != 1) {
        LOG_ERROR("Failed to load TLS private key: " + m_tlsKeyFile + " (" + getOpenSslErrorMessage() + ")");
        destroyTlsContext();
        return false;
    }

    if (SSL_CTX_check_private_key(m_tlsContext) != 1) {
        LOG_ERROR("TLS certificate and private key do not match: " + getOpenSslErrorMessage());
        destroyTlsContext();
        return false;
    }

    return true;
}

/**
 * @brief 释放 TLS 上下文
 */
void HttpServer::destroyTlsContext() {
    if (m_tlsContext) {
        SSL_CTX_free(m_tlsContext);
        m_tlsContext = nullptr;
    }
}

/**
 * @brief 创建单个 TLS 会话
 */
std::shared_ptr<SSL> HttpServer::createTlsSession(int clientSocket) const {
    if (!m_tlsEnabled || !m_tlsContext) {
        return nullptr;
    }

    SSL* rawSession = SSL_new(m_tlsContext);
    if (!rawSession) {
        LOG_ERROR("Failed to create TLS session for fd " + std::to_string(clientSocket) + ": " + getOpenSslErrorMessage());
        return nullptr;
    }

    if (SSL_set_fd(rawSession, clientSocket) != 1) {
        LOG_ERROR("Failed to bind TLS session to fd " + std::to_string(clientSocket) + ": " + getOpenSslErrorMessage());
        SSL_free(rawSession);
        return nullptr;
    }

    SSL_set_accept_state(rawSession);

    return std::shared_ptr<SSL>(rawSession, [](SSL* session) {
        if (session) {
            SSL_free(session);
        }
    });
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

    if (m_tcpServer) {
        m_tcpServer->stop();
    }

    if (m_threadPool) {
        m_threadPool->shutdown();
        m_threadPool.reset();
    }

    if (m_fileCache) {
        m_fileCache.reset();
    }

    {
        std::lock_guard<std::mutex> lock(m_clientInfoMutex);
        for (auto& pair : m_clientInfoMap) {
            clearSocketReadBuffer(pair.first);
            close(pair.first);
        }
        m_clientInfoMap.clear();
    }

    m_tcpServer.reset();

    destroyTlsContext();

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
 * @brief 配置 TLS/HTTPS 选项
 */
void HttpServer::setTlsConfig(bool enabled,
                              const std::string& certFile,
                              const std::string& keyFile,
                              const std::string& cipherSuites) {
    m_tlsEnabled = enabled;
    m_tlsCertFile = certFile;
    m_tlsKeyFile = keyFile;
    m_tlsCipherSuites = cipherSuites;
}

/**
 * @brief 获取缓存统计信息
 *
 * @return std::string 缓存统计信息的JSON格式字符串
 */
nlohmann::json HttpServer::getCacheStats() const {
    if (!m_fileCache) {
        return nlohmann::json::object();
    }

    nlohmann::json stats;
    stats["enabled"] = m_fileCache->isEnabled();
    stats["maxSize"] = m_fileCache->getMaxSize();
    stats["currentSize"] = m_fileCache->getCurrentSize();
    stats["maxFileSize"] = m_fileCache->getMaxFileSize();
    stats["cacheCount"] = m_fileCache->getCacheCount();
    stats["hitCount"] = m_fileCache->getHitCount();
    stats["missCount"] = m_fileCache->getMissCount();
    stats["hitRate"] = m_fileCache->getHitRate();
    return stats;
}

/**
 * @brief 清空文件缓存
 */
void HttpServer::clearCache() {
    if (m_fileCache) {
        m_fileCache->clear();
    }
}

void HttpServer::addMiddleware(Middleware middleware) {
    if (middleware) {
        m_middlewares.push_back(std::move(middleware));
    }
}

void HttpServer::clearMiddlewares() {
    m_middlewares.clear();
}

std::string HttpServer::buildRouteKey(HttpRequest::Method method, const std::string& path) const {
    return HttpRequest::methodToString(method) + " " + path;
}

bool HttpServer::matchRoutePattern(const std::string& pattern,
                                   const std::string& path,
                                   std::map<std::string, std::string>& pathParams) const {
    const auto patternSegments = splitPathSegments(pattern);
    const auto pathSegments = splitPathSegments(path);

    if (patternSegments.size() != pathSegments.size()) {
        return false;
    }

    pathParams.clear();
    for (size_t index = 0; index < patternSegments.size(); ++index) {
        const std::string& patternSegment = patternSegments[index];
        const std::string& pathSegment = pathSegments[index];

        if (patternSegment.size() >= 3 && patternSegment.front() == '{' && patternSegment.back() == '}') {
            pathParams[patternSegment.substr(1, patternSegment.size() - 2)] = pathSegment;
            continue;
        }

        if (patternSegment != pathSegment) {
            return false;
        }
    }

    return true;
}

bool HttpServer::matchRouteParams(const HttpRequest& request,
                                  const std::map<std::string, std::string>& requiredParams) const {
    for (const auto& requiredParam : requiredParams) {
        if (request.getParameter(requiredParam.first) != requiredParam.second) {
            return false;
        }
    }

    return true;
}

std::string HttpServer::getAllowedMethodsForPath(const std::string& path) const {
    std::vector<std::string> methods;
    for (const auto& pair : m_routeHandlers) {
        const std::string suffix = " " + path;
        if (pair.first.size() > suffix.size() &&
            pair.first.compare(pair.first.size() - suffix.size(), suffix.size(), suffix) == 0) {
            const std::string method = pair.first.substr(0, pair.first.size() - suffix.size());
            for (size_t index = 0; index < pair.second.size(); ++index) {
                methods.push_back(method);
            }
        }
    }

    for (const auto& routePattern : m_routePatterns) {
        std::map<std::string, std::string> pathParams;
        if (matchRoutePattern(routePattern.pattern, path, pathParams)) {
            methods.push_back(HttpRequest::methodToString(routePattern.method));
        }
    }

    if (methods.empty()) {
        return "GET, HEAD, OPTIONS";
    }

    std::sort(methods.begin(), methods.end());
    methods.erase(std::unique(methods.begin(), methods.end()), methods.end());

    std::ostringstream allow;
    for (size_t index = 0; index < methods.size(); ++index) {
        if (index > 0) {
            allow << ", ";
        }
        allow << methods[index];
    }

    if (std::find(methods.begin(), methods.end(), "OPTIONS") == methods.end()) {
        if (!methods.empty()) {
            allow << ", ";
        }
        allow << "OPTIONS";
    }

    return allow.str();
}

void HttpServer::registerRoute(HttpRequest::Method method,
                               const std::string& path,
                               RequestHandler handler,
                               const std::map<std::string, std::string>& requiredParams) {
    m_routeHandlers[buildRouteKey(method, path)].push_back({std::move(handler), requiredParams});
}

void HttpServer::registerRoutePattern(HttpRequest::Method method,
                                      const std::string& pathPattern,
                                      RequestHandler handler,
                                      const std::map<std::string, std::string>& requiredParams) {
    m_routePatterns.push_back({method, pathPattern, std::move(handler), requiredParams});
}

void HttpServer::registerDefaultRoutes() {
    m_routeHandlers.clear();
    m_routePatterns.clear();

    registerRoute(HttpRequest::METHOD_GET, "/api/echo", [this](const HttpRequest& request) {
        return handleApiEcho(request);
    });
    registerRoute(HttpRequest::METHOD_HEAD, "/api/echo", [this](const HttpRequest& request) {
        return handleApiEcho(request);
    });
    registerRoute(HttpRequest::METHOD_POST, "/api/echo", [this](const HttpRequest& request) {
        return handleApiEcho(request);
    });

    registerRoute(HttpRequest::METHOD_GET, "/health", [this](const HttpRequest&) {
        return handleHealthCheck();
    });
    registerRoute(HttpRequest::METHOD_HEAD, "/health", [this](const HttpRequest&) {
        return handleHealthCheck();
    });

    registerRoute(HttpRequest::METHOD_GET, "/status", [this](const HttpRequest&) {
        return handleStatusRequest();
    });
    registerRoute(HttpRequest::METHOD_HEAD, "/status", [this](const HttpRequest&) {
        return handleStatusRequest();
    });
}

bool HttpServer::dispatchRoute(const HttpRequest& request, HttpResponse& response) const {
    const std::string routeKey = buildRouteKey(request.getMethod(), request.getPath());
    const auto it = m_routeHandlers.find(routeKey);
    if (it != m_routeHandlers.end()) {
        size_t bestMatchSpecificity = 0;
        bool matched = false;

        for (const auto& routeEntry : it->second) {
            if (!matchRouteParams(request, routeEntry.requiredParams)) {
                continue;
            }

            const size_t specificity = routeEntry.requiredParams.size();
            if (matched && specificity <= bestMatchSpecificity) {
                continue;
            }

            response = routeEntry.handler(request);
            bestMatchSpecificity = specificity;
            matched = true;
        }

        if (matched) {
            return true;
        }
    }

    size_t bestMatchSpecificity = 0;
    bool matched = false;
    HttpResponse bestResponse;

    for (const auto& routePattern : m_routePatterns) {
        if (routePattern.method != request.getMethod()) {
            continue;
        }

        std::map<std::string, std::string> pathParams;
        if (!matchRoutePattern(routePattern.pattern, request.getPath(), pathParams)) {
            continue;
        }

        if (!matchRouteParams(request, routePattern.requiredParams)) {
            continue;
        }

        HttpRequest routedRequest = request;
        routedRequest.setPathParams(pathParams);
        const size_t specificity = routePattern.requiredParams.size();
        if (matched && specificity <= bestMatchSpecificity) {
            continue;
        }

        bestResponse = routePattern.handler(routedRequest);
        bestMatchSpecificity = specificity;
        matched = true;
    }

    if (matched) {
        response = std::move(bestResponse);
        return true;
    }

    return false;
}

void HttpServer::runMiddlewareChain(size_t index,
                                    const HttpRequest& request,
                                    HttpResponse& response,
                                    const std::function<void()>& finalHandler) const {
    if (index >= m_middlewares.size()) {
        finalHandler();
        return;
    }

    const Middleware& middleware = m_middlewares[index];
    middleware(request, response, [this, index, &request, &response, &finalHandler]() {
        runMiddlewareChain(index + 1, request, response, finalHandler);
    });
}

void HttpServer::processRequest(HttpRequest& request, HttpResponse& response) const {
    const std::map<std::string, std::string> middlewareHeaders = response.getHeaders();

    HttpResponse generatedResponse;
    if (m_requestHandler) {
        generatedResponse = m_requestHandler(request);
    } else if (!dispatchRoute(request, generatedResponse)) {
        if (request.getMethod() == HttpRequest::METHOD_GET ||
            request.getMethod() == HttpRequest::METHOD_HEAD) {
            generatedResponse = handleStaticFile(request);
        } else if (request.getMethod() == HttpRequest::METHOD_POST) {
            generatedResponse = handlePostRequest(request);
        } else if (request.getMethod() == HttpRequest::METHOD_OPTIONS) {
            generatedResponse.setStatusCode(HttpResponse::STATUS_204_NO_CONTENT);
            generatedResponse.addHeader("Allow", getAllowedMethodsForPath(request.getPath()));
            generatedResponse.setBody("");
        } else if (request.getMethod() == HttpRequest::METHOD_PATCH ||
               request.getMethod() == HttpRequest::METHOD_PUT ||
                   request.getMethod() == HttpRequest::METHOD_DELETE) {
            generatedResponse = buildUnifiedErrorResponse(request,
                                                          HttpResponse::STATUS_405_METHOD_NOT_ALLOWED,
                                                          "",
                                                          getAllowedMethodsForPath(request.getPath()));
        } else {
            generatedResponse = buildUnifiedErrorResponse(request,
                                                          HttpResponse::STATUS_501_NOT_IMPLEMENTED,
                                                          "Unsupported HTTP method");
        }
    }

    for (const auto& header : middlewareHeaders) {
        if (generatedResponse.getHeader(header.first).empty()) {
            generatedResponse.addHeader(header.first, header.second);
        }
    }

    response = std::move(generatedResponse);
}

HttpResponse HttpServer::buildUnifiedErrorResponse(const HttpRequest& request,
                                                   int code,
                                                   const std::string& detail,
                                                   const std::string& allowMethods) const {
    const auto statusCode = static_cast<HttpResponse::StatusCode>(code);

    if (containsJsonAcceptHeader(request)) {
        HttpResponse response = HttpResponse::jsonError(statusCode, detail.empty() ? HttpResponse::statusCodeToString(statusCode) : detail);
        if (!allowMethods.empty()) {
            response.addHeader("Allow", allowMethods);
        }
        return response;
    }

    switch (statusCode) {
        case HttpResponse::STATUS_400_BAD_REQUEST:
            return HttpResponse::badRequest();
        case HttpResponse::STATUS_404_NOT_FOUND:
            return HttpResponse::notFound();
        case HttpResponse::STATUS_405_METHOD_NOT_ALLOWED:
            return allowMethods.empty() ? HttpResponse::methodNotAllowed() : HttpResponse::methodNotAllowed(allowMethods);
        case HttpResponse::STATUS_500_INTERNAL_SERVER_ERROR:
            return HttpResponse::internalServerError();
        case HttpResponse::STATUS_501_NOT_IMPLEMENTED:
            return HttpResponse::notImplemented();
        default: {
            HttpResponse response;
            response.setStatusCode(statusCode);
            response.setContentType("text/plain; charset=utf-8");
            response.setBody(detail.empty() ? HttpResponse::statusCodeToString(statusCode) : detail);
            return response;
        }
    }
}

HttpResponse HttpServer::handleRequest(HttpRequest request) const {
    HttpResponse response;

    try {
        auto finalHandler = [&]() {
            processRequest(request, response);
        };

        runMiddlewareChain(0, request, response, finalHandler);

        if (request.getMethod() == HttpRequest::METHOD_HEAD) {
            stripResponseBodyForHead(response);
        }
    } catch (const std::exception& e) {
        response = buildUnifiedErrorResponse(request,
                                             HttpResponse::STATUS_500_INTERNAL_SERVER_ERROR,
                                             e.what());
    }

    return response;
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
    if (m_tcpServer &&
        m_tcpServer->getServerSocket() >= 0 &&
        getsockname(m_tcpServer->getServerSocket(), (struct sockaddr*)&addr, &addrLen) == 0) {
        // 将网络字节序的IP地址转换为字符串格式
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        return std::string(ip);
    }
    
    return "0.0.0.0";  // 出错时返回默认值
}

/**
 * @brief 处理已接受的客户端连接
 * 
 * TcpServer只负责accept，本方法负责将连接交给HTTP层：
 * - epoll模式下注册到fd监听
 * - 传统模式下直接进入线程池处理
 */
void HttpServer::handleClientAccepted(int clientSocket, const std::string& clientIp, int clientPort) {
    LOG_INFO("Client connected: " + clientIp + ":" + std::to_string(clientPort));

    std::shared_ptr<SSL> tlsSession;
    if (m_tlsEnabled) {
        tlsSession = createTlsSession(clientSocket);
        if (!tlsSession) {
            clearSocketReadBuffer(clientSocket);
            close(clientSocket);
            return;
        }
    }

    ClientInfo clientInfo{clientIp, clientPort, "", tlsSession};

    if (!m_useEpoll) {
        if (!m_threadPool) {
            close(clientSocket);
            return;
        }

        m_threadPool->enqueue([this, clientSocket, clientInfo]() mutable {
            while (this->m_running.load()) {
                bool keepAlive = this->handleClient(clientSocket,
                                                    clientInfo.ip,
                                                    clientInfo.port,
                                                    clientInfo.tlsSession.get());
                if (!keepAlive) {
                    break;
                }
            }

            clearSocketReadBuffer(clientSocket);
            close(clientSocket);
        });
        return;
    }

    if (!m_tcpServer) {
        close(clientSocket);
        return;
    }

    if (!TcpServer::setNonBlocking(clientSocket)) {
        LOG_ERROR("Failed to set client socket to non-blocking mode");
        close(clientSocket);
        return;
    }

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    int nodelay = 1;
    setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    {
        std::lock_guard<std::mutex> lock(m_clientInfoMutex);
        m_clientInfoMap[clientSocket] = std::move(clientInfo);
    }

    auto clientCallback = [this](int fd, uint32_t ev) {
        this->handleClientRead(fd, ev);
    };

    if (!m_tcpServer->addFd(clientSocket, EpollEventType::READ, clientCallback, false)) {
        LOG_ERROR("Failed to add client socket to epoll");
        clearSocketReadBuffer(clientSocket);
        close(clientSocket);
        std::lock_guard<std::mutex> lock(m_clientInfoMutex);
        m_clientInfoMap.erase(clientSocket);
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
    if (m_tcpServer) {
        m_tcpServer->removeFd(clientSocket);
    }

    // 将socket设置为阻塞模式，确保能完整读取HTTP请求
    // 因为此时已经从epoll移除，不再需要非阻塞
    int flags = fcntl(clientSocket, F_GETFL, 0);
    if (flags != -1) {
        fcntl(clientSocket, F_SETFL, flags & ~O_NONBLOCK);
    }

    // 将请求处理提交到线程池
    m_threadPool->enqueue([this, clientSocket, clientInfo]() {
        // 在线程池中处理请求
        bool keepAlive = this->handleClient(clientSocket,
                            clientInfo.ip,
                            clientInfo.port,
                            clientInfo.tlsSession.get());

        if (keepAlive) {
            // Keep-Alive：重新注册到epoll，等待下一个请求
            // 将socket恢复为非阻塞模式
            int flags = fcntl(clientSocket, F_GETFL, 0);
            if (flags != -1) {
                fcntl(clientSocket, F_SETFL, flags | O_NONBLOCK);
            }

            // 重新将客户端信息加入map
            {
                std::lock_guard<std::mutex> lock(this->m_clientInfoMutex);
                this->m_clientInfoMap[clientSocket] = clientInfo;
            }

            // 重新注册到epoll
            auto clientCallback = [this](int fd, uint32_t events) {
                this->handleClientRead(fd, events);
            };
            if (this->m_running.load() && this->m_tcpServer &&
                this->m_tcpServer->addFd(clientSocket, EpollEventType::READ, clientCallback, false)) {
                std::lock_guard<std::mutex> lock(this->m_clientInfoMutex);
                this->m_clientInfoMap[clientSocket] = clientInfo;
            } else {
                {
                    std::lock_guard<std::mutex> lock(this->m_clientInfoMutex);
                    this->m_clientInfoMap.erase(clientSocket);
                }
                clearSocketReadBuffer(clientSocket);
                close(clientSocket);
            }
        } else {
            // 非Keep-Alive或出错：关闭socket
            clearSocketReadBuffer(clientSocket);
            close(clientSocket);
        }
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
    if (m_tcpServer) {
        m_tcpServer->removeFd(clientSocket);
    }
    // 关闭socket
    clearSocketReadBuffer(clientSocket);
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
 * 根据请求路径查找对应的文件，并生成HTTP响应。
 * 该流程负责静态资源的完整协商与回退，包括：
 * - 路径遍历检查与 URL 解码后的二次校验
 * - 目录请求自动解析 index.html
 * - 目录无索引页时返回目录列表
 * - 生成 ETag 和 Last-Modified，用于缓存验证
 * - 处理 If-None-Match / If-Modified-Since 并返回 304
 * - 支持单段 Range 请求并返回 206 / 416
 *
 * 这是静态文件链路的核心入口，既承担资源发现，也承担协议协商。
 * 5. 关闭客户端连接
 *
 * @param clientSocket 客户端socket描述符
 * @param clientIp 客户端IP地址
 * @param clientPort 客户端端口号
 */
bool HttpServer::handleClient(int clientSocket,
                              const std::string& clientIp,
                              int clientPort,
                              SSL* ssl) {
    if (ssl && !SSL_is_init_finished(ssl)) {
        while (true) {
            int acceptResult = SSL_accept(ssl);
            if (acceptResult == 1) {
                break;
            }

            int sslError = SSL_get_error(ssl, acceptResult);
            if (sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE) {
                continue;
            }

            if (sslError == SSL_ERROR_ZERO_RETURN) {
                LOG_INFO("TLS client closed during handshake: " + clientIp + ":" + std::to_string(clientPort));
                return false;
            }

            LOG_ERROR("TLS handshake failed for " + clientIp + ":" + std::to_string(clientPort) + ": " + getOpenSslErrorMessage());
            return false;
        }
    }

    // 记录请求开始时间
    auto startTime = std::chrono::steady_clock::now();

    // 初始化请求和响应对象
    HttpRequest request;
    request.setClientIp(clientIp);
    request.setClientPort(clientPort);

    HttpResponse response;
    bool parseSuccess = false;
    bool keepAlive = false;
    size_t declaredResponseSize = 0;

    try {
        // 步骤1：解析HTTP请求
        request = parseRequest(clientSocket, ssl);

        // 检查请求是否有效（如果解析失败，request会是默认构造的无效对象）
        if (request.getUrl().empty()) {
            response = buildUnifiedErrorResponse(request, HttpResponse::STATUS_400_BAD_REQUEST, "Invalid request");
            declaredResponseSize = response.getBodySize();
            LOG_WARN("Invalid request from " + clientIp + ":" + std::to_string(clientPort));
        } else if (request.getMethod() == HttpRequest::METHOD_UNKNOWN) {
            response = buildUnifiedErrorResponse(request,
                                                 HttpResponse::STATUS_501_NOT_IMPLEMENTED,
                                                 "Unsupported HTTP method");
            declaredResponseSize = response.getBodySize();
            LOG_WARN("Unsupported HTTP method from " + clientIp + ":" + std::to_string(clientPort));
        } else {
            parseSuccess = true;
            runMiddlewareChain(0, request, response, [&]() {
                processRequest(request, response);
            });
            declaredResponseSize = response.getBodySize();

            if (request.getMethod() == HttpRequest::METHOD_HEAD) {
                stripResponseBodyForHead(response);
            }
        }
    } catch (const std::exception& e) {
        // 捕获异常并返回500错误
        LOG_ERROR("Request handling exception: " + std::string(e.what()));
        response = buildUnifiedErrorResponse(request,
                                             HttpResponse::STATUS_500_INTERNAL_SERVER_ERROR,
                                             e.what());
        declaredResponseSize = response.getBodySize();
        if (request.getMethod() == HttpRequest::METHOD_HEAD) {
            stripResponseBodyForHead(response);
        }
    }

    // 步骤3：判断是否保持连接（Keep-Alive）
    if (parseSuccess) {
        std::string connHeader = request.getHeader("Connection");
        std::string version = request.getVersion();

        // HTTP/1.1 默认 Keep-Alive，除非 Connection: close
        // HTTP/1.0 默认关闭，除非 Connection: keep-alive
        if (version == "HTTP/1.1") {
            keepAlive = (connHeader != "close");
        } else {
            keepAlive = (connHeader == "keep-alive");
        }

        // 添加 Connection 响应头
        if (keepAlive) {
            response.addHeader("Connection", "keep-alive");
            response.addHeader("Keep-Alive", "timeout=5, max=100");
        } else {
            response.addHeader("Connection", "close");
        }
    }

    // 步骤4：准备响应体
    size_t responseSize = declaredResponseSize;
    std::string bodyToSend;
    bool useCache = false;
    bool useSendfile = false;  // 是否使用sendfile优化
    int fileFd = -1;           // 文件描述符（用于sendfile）
    off_t fileOffset = 0;      // 文件偏移量
    size_t fileSize = 0;       // 文件大小
    size_t fullFileSize = 0;   // 文件总大小（用于Range与缓存逻辑）

    // 对静态文件响应支持单段Range请求
    if (parseSuccess && !response.getFilePath().empty() &&
        response.getStatusCode() == HttpResponse::STATUS_200_OK) {
        response.addHeader("Accept-Ranges", "bytes");

        const std::string rangeHeader = request.getHeader("Range");
        if (!rangeHeader.empty()) {
            struct stat st;
            if (stat(response.getFilePath().c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                off_t rangeStart = 0;
                off_t rangeEnd = 0;
                RangeParseResult parseResult = parseSingleRangeHeader(rangeHeader, st.st_size, rangeStart, rangeEnd);

                if (parseResult == RangeParseResult::VALID) {
                    response.setStatusCode(HttpResponse::STATUS_206_PARTIAL_CONTENT);
                    response.addHeader("Content-Range",
                                       "bytes " + std::to_string(rangeStart) + "-" +
                                       std::to_string(rangeEnd) + "/" +
                                       std::to_string(static_cast<long long>(st.st_size)));
                } else if (parseResult == RangeParseResult::INVALID) {
                    response.setStatusCode(HttpResponse::STATUS_416_RANGE_NOT_SATISFIABLE);
                    response.addHeader("Content-Range",
                                       "bytes */" + std::to_string(static_cast<long long>(st.st_size)));
                    response.setBody("");
                    response.setFilePath("");
                }
            }
        }
    }

    if (!response.getFilePath().empty() &&
        (response.getStatusCode() == HttpResponse::STATUS_200_OK ||
         response.getStatusCode() == HttpResponse::STATUS_206_PARTIAL_CONTENT)) {
        off_t rangeStart = 0;
        off_t rangeEnd = 0;
        bool isPartialContent = false;

        if (response.getStatusCode() == HttpResponse::STATUS_206_PARTIAL_CONTENT) {
            const std::string rangeHeader = request.getHeader("Range");
            struct stat st;
            if (stat(response.getFilePath().c_str(), &st) == 0) {
                isPartialContent =
                    (parseSingleRangeHeader(rangeHeader, st.st_size, rangeStart, rangeEnd) == RangeParseResult::VALID);
            }
        }

        // 优先尝试从缓存获取文件内容
        if (m_fileCache && m_fileCache->isEnabled()) {
            auto cachedContent = m_fileCache->get(response.getFilePath());
            if (cachedContent) {
                // 缓存命中，使用缓存内容
                if (isPartialContent) {
                    size_t start = static_cast<size_t>(rangeStart);
                    size_t end = static_cast<size_t>(rangeEnd);
                    if (start < cachedContent->size() && end >= start) {
                        size_t len = end - start + 1;
                        bodyToSend = cachedContent->substr(start, len);
                        responseSize = bodyToSend.size();
                        useCache = true;
                    }
                } else {
                    bodyToSend = *cachedContent;
                    responseSize = cachedContent->size();
                    useCache = true;
                }
            }
        }

        if (!useCache) {
            // 缓存未命中，使用sendfile零拷贝优化发送文件
            // sendfile直接在内核空间将文件内容发送到socket，避免用户空间拷贝
            fileFd = open(response.getFilePath().c_str(), O_RDONLY);
            if (fileFd >= 0) {
                struct stat fileStat;
                if (fstat(fileFd, &fileStat) == 0) {
                    fullFileSize = fileStat.st_size;
                    if (isPartialContent) {
                        fileOffset = rangeStart;
                        responseSize = static_cast<size_t>(rangeEnd - rangeStart + 1);
                        fileSize = responseSize;
                    } else {
                        responseSize = fullFileSize;
                        fileSize = fullFileSize;
                    }
                    useSendfile = true;

                    // 如果文件较小，仍然使用缓存
                    if (m_fileCache && m_fileCache->isEnabled() &&
                        fullFileSize <= m_fileCache->getMaxFileSize()) {
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

    // 步骤5：发送HTTP响应
    std::string headerStr = response.buildHeaderString(responseSize);
    if (sendData(clientSocket, headerStr.c_str(), headerStr.size(), ssl) <= 0) {
        keepAlive = false;  // 发送失败，关闭连接
    }

    const bool shouldSendBody = !(parseSuccess &&
                                  (request.getMethod() == HttpRequest::METHOD_HEAD ||
                                   response.getStatusCode() == HttpResponse::STATUS_204_NO_CONTENT ||
                                   response.getStatusCode() == HttpResponse::STATUS_304_NOT_MODIFIED));

    if (shouldSendBody && ssl && fileFd >= 0) {
        if (lseek(fileFd, fileOffset, SEEK_SET) < 0) {
            keepAlive = false;
        } else {
            char buffer[8192];
            size_t remaining = fileSize;
            while (remaining > 0) {
                size_t chunkSize = std::min(remaining, sizeof(buffer));
                ssize_t bytesRead = read(fileFd, buffer, chunkSize);
                if (bytesRead < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    keepAlive = false;
                    break;
                }
                if (bytesRead == 0) {
                    break;
                }

                if (sendData(clientSocket, buffer, static_cast<size_t>(bytesRead), ssl) <= 0) {
                    keepAlive = false;
                    break;
                }

                remaining -= static_cast<size_t>(bytesRead);
            }
        }
        close(fileFd);
    } else if (shouldSendBody && useSendfile && fileFd >= 0) {
        // 使用sendfile零拷贝发送文件内容，性能更优
        ssize_t sent;
        while (fileSize > 0) {
            sent = sendfile(clientSocket, fileFd, &fileOffset, fileSize);
            if (sent <= 0) {
                if (errno == EINTR) {
                    continue;  // 被信号中断，重试
                }
                keepAlive = false;  // 发送失败，关闭连接
                break;
            }
            fileSize -= sent;
        }
        close(fileFd);
    } else if (fileFd >= 0) {
        close(fileFd);
    } else if (shouldSendBody && !bodyToSend.empty()) {
        // 使用缓存内容或普通响应体发送
        if (sendData(clientSocket, bodyToSend.c_str(), bodyToSend.size()) <= 0) {
            keepAlive = false;  // 发送失败，关闭连接
        }
    }

    // 计算处理耗时
    auto endTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
    double durationMs = duration.count() / 1000.0;

    // 记录访问日志
    std::string method = HttpRequest::methodToString(request.getMethod());
    std::string url = request.getUrl();
    int statusCode = response.getStatusCode();

    LOG_ACCESS(clientIp, method, url, statusCode, responseSize, durationMs);

    return keepAlive;
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
HttpRequest HttpServer::parseRequest(int clientSocket, SSL* ssl) const {
    HttpRequest request;
    std::string line;

    // 性能优化：删除多余的 select() 调用
    // epoll 水平触发（LT）模式已确保数据就绪才调用此函数，无需再用 select() 二次确认

    // 读取请求行（第一行）
    if (readLine(clientSocket, line, ssl) <= 0) {
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

    // 设置请求方法、URL和HTTP版本
    request.setMethodString(method);
    request.setUrl(url);
    request.setVersion(version);

    // 读取HTTP头部
    while (readLine(clientSocket, line, ssl) > 0) {
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
            readData(clientSocket, &body[0], contentLength, ssl);
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

    if (!S_ISREG(st.st_mode)) {
        return HttpResponse::notFound();
    }

    const std::string etag = buildStaticFileEtag(st);
    const std::string lastModified = formatHttpDate(st.st_mtime);

    if (isConditionalNotModified(request, st, etag)) {
        HttpResponse response;
        response.setStatusCode(HttpResponse::STATUS_304_NOT_MODIFIED);
        response.addHeader("ETag", etag);
        response.addHeader("Last-Modified", lastModified);
        response.addHeader("Accept-Ranges", "bytes");
        return response;
    }

    // 创建成功响应
    HttpResponse response;
    response.setStatusCode(HttpResponse::STATUS_200_OK);
    response.setFilePath(filePath);  // 设置文件路径，响应类会自动识别Content-Type
    response.addHeader("ETag", etag);
    response.addHeader("Last-Modified", lastModified);
    response.addHeader("Accept-Ranges", "bytes");

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
int HttpServer::readLine(int socket, std::string& line, SSL* ssl) const {
    line.clear();

    // 先消费已有缓冲，再按块读取并持续尝试切行
    while (true) {
        {
            std::lock_guard<std::mutex> lock(g_readBufferMutex);
            auto it = g_socketReadBuffers.find(socket);
            if (it != g_socketReadBuffers.end()) {
                std::string& buffer = it->second;
                size_t newlinePos = buffer.find('\n');
                if (newlinePos != std::string::npos) {
                    line.assign(buffer.data(), newlinePos);
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }

                    buffer.erase(0, newlinePos + 1);
                    if (buffer.empty()) {
                        g_socketReadBuffers.erase(it);
                    }
                    return static_cast<int>(line.length());
                }
            }
        }

        char chunk[4096];
        ssize_t n = ssl ? SSL_read(ssl, chunk, sizeof(chunk)) : read(socket, chunk, sizeof(chunk));
        if (n < 0) {
            if (ssl) {
                int sslError = SSL_get_error(ssl, static_cast<int>(n));
                if (sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE) {
                    continue;
                }

                if (sslError == SSL_ERROR_ZERO_RETURN) {
                    std::lock_guard<std::mutex> lock(g_readBufferMutex);
                    auto it = g_socketReadBuffers.find(socket);
                    if (it != g_socketReadBuffers.end() && !it->second.empty()) {
                        line = it->second;
                        if (!line.empty() && line.back() == '\r') {
                            line.pop_back();
                        }
                        g_socketReadBuffers.erase(it);
                        return static_cast<int>(line.length());
                    }
                    return -1;
                }
            }

            if (errno == EINTR) {
                continue;
            }

            // 超时/无更多数据时，若已有残留缓冲则按旧语义返回部分行
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::lock_guard<std::mutex> lock(g_readBufferMutex);
                auto it = g_socketReadBuffers.find(socket);
                if (it != g_socketReadBuffers.end() && !it->second.empty()) {
                    line = it->second;
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    g_socketReadBuffers.erase(it);
                    return static_cast<int>(line.length());
                }
                return -1;
            }

            // 其他错误：有残留则返回部分行，否则失败
            std::lock_guard<std::mutex> lock(g_readBufferMutex);
            auto it = g_socketReadBuffers.find(socket);
            if (it != g_socketReadBuffers.end() && !it->second.empty()) {
                line = it->second;
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                g_socketReadBuffers.erase(it);
                return static_cast<int>(line.length());
            }
            return -1;
        }

        if (n == 0) {
            // 连接关闭：把残留缓冲作为最后一行返回
            std::lock_guard<std::mutex> lock(g_readBufferMutex);
            auto it = g_socketReadBuffers.find(socket);
            if (it != g_socketReadBuffers.end() && !it->second.empty()) {
                line = it->second;
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                g_socketReadBuffers.erase(it);
                return static_cast<int>(line.length());
            }
            return -1;
        }

        {
            std::lock_guard<std::mutex> lock(g_readBufferMutex);
            g_socketReadBuffers[socket].append(chunk, static_cast<size_t>(n));
        }
    }
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
ssize_t HttpServer::readData(int socket, char* buffer, size_t size, SSL* ssl) const {
    size_t totalRead = 0;
    ssize_t n;

    // 循环读取直到达到指定数量
    while (totalRead < size) {
        n = ssl ? SSL_read(ssl, buffer + totalRead, static_cast<int>(size - totalRead))
                : read(socket, buffer + totalRead, size - totalRead);
        if (n < 0) {
            if (ssl) {
                int sslError = SSL_get_error(ssl, static_cast<int>(n));
                if (sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE) {
                    continue;
                }
            }

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
ssize_t HttpServer::sendData(int socket, const char* data, size_t size, SSL* ssl) const {
    size_t totalSent = 0;
    ssize_t n;

    // 循环发送直到全部发送完成
    while (totalSent < size) {
        n = ssl ? SSL_write(ssl, data + totalSent, static_cast<int>(size - totalSent))
                : write(socket, data + totalSent, size - totalSent);
        
        if (n <= 0) {
            if (ssl) {
                int sslError = SSL_get_error(ssl, static_cast<int>(n));
                if (sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE) {
                    continue;
                }
            }

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
    const auto formData = request.parseFormData();
    const auto jsonBody = request.parseJsonBody();
    nlohmann::json response;
    response["status"] = "success";
    response["message"] = "POST request received";
    response["path"] = request.getPath();
    response["bodyLength"] = request.getBody().length();
    response["contentType"] = request.getContentType();

    if (!jsonBody.empty()) {
        response["jsonBody"] = parsedValueMapToJsonObject(jsonBody);
    } else {
        response["formData"] = mapToJsonObject(formData);
    }

    return HttpResponse::jsonResponse(HttpResponse::STATUS_200_OK, response.dump(2));
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
    nlohmann::json response;
    response["method"] = HttpRequest::methodToString(request.getMethod());
    response["path"] = request.getPath();
    response["url"] = request.getUrl();
    response["queryParams"] = mapToJsonObject(request.parseQueryParams());
    if (!request.getPathParams().empty()) {
        response["pathParams"] = mapToJsonObject(request.getPathParams());
    }

    if (request.getMethod() == HttpRequest::METHOD_POST) {
        response["contentType"] = request.getContentType();
        response["body"] = request.getBody();

        const auto jsonBody = request.parseJsonBody();
        if (!jsonBody.empty()) {
            response["jsonBody"] = parsedValueMapToJsonObject(jsonBody);
        } else {
            response["formData"] = mapToJsonObject(request.parseFormData());
        }
    }

    return HttpResponse::jsonResponse(HttpResponse::STATUS_200_OK, response.dump(2));
}

HttpResponse HttpServer::handleHealthCheck() const {
    nlohmann::json response;
    response["status"] = "ok";
    response["service"] = "CppHttpServer";
    response["routeCount"] = m_routeHandlers.size();
    response["patternRouteCount"] = m_routePatterns.size();
    response["middlewareCount"] = m_middlewares.size();

    return HttpResponse::jsonResponse(HttpResponse::STATUS_200_OK, response.dump(2));
}

HttpResponse HttpServer::handleStatusRequest() const {
    nlohmann::json response;
    response["running"] = m_running.load();
    response["ip"] = m_ip;
    response["port"] = m_port;
    response["docRoot"] = m_docRoot;
    response["threads"] = m_numThreads;
    response["cacheEnabled"] = isCacheEnabled();
    response["cacheStats"] = getCacheStats();
    response["routes"] = m_routeHandlers.size();
    response["patternRoutes"] = m_routePatterns.size();
    response["middlewares"] = m_middlewares.size();

    return HttpResponse::jsonResponse(HttpResponse::STATUS_200_OK, response.dump(2));
}
