/**
 * @file http_response.cc
 * @brief HTTP响应类的实现文件
 * 
 * 实现HttpResponse类中声明的所有方法，
 * 包括响应构建、状态码处理、Content-Type识别等功能。
 */

#include "../response/http_response.h"
#include <cstring>         // C字符串处理
#include <sys/stat.h>      // 文件状态
#include <iostream>        // 输入输出

/**
 * @brief 构造函数
 * 
 * 创建一个新的HttpResponse对象，初始化为默认的200 OK状态。
 */
HttpResponse::HttpResponse()
    : m_statusCode(STATUS_200_OK),  // 默认状态码
      m_statusMessage("OK") {       // 默认状态消息
}

/**
 * @brief 析构函数
 * 
 * 销毁HttpResponse对象。
 * 由于使用标准库容器，析构函数自动处理资源释放。
 */
HttpResponse::~HttpResponse() {
}

/**
 * @brief 设置HTTP状态码
 * 
 * 设置响应的状态码，同时自动更新对应的状态消息。
 * 
 * @param code StatusCode枚举值
 */
void HttpResponse::setStatusCode(StatusCode code) {
    m_statusCode = code;
    m_statusMessage = statusCodeToString(code);  // 自动更新状态消息
}

/**
 * @brief 获取HTTP状态码
 * 
 * @return StatusCode 当前状态码
 */
HttpResponse::StatusCode HttpResponse::getStatusCode() const {
    return m_statusCode;
}

/**
 * @brief 设置状态消息
 * 
 * 自定义状态消息文本。
 * 通常不需要手动调用，setStatusCode会自动设置标准消息。
 * 
 * @param message 状态消息
 */
void HttpResponse::setStatusMessage(const std::string& message) {
    m_statusMessage = message;
}

/**
 * @brief 获取状态消息
 * 
 * @return std::string 当前状态消息
 */
std::string HttpResponse::getStatusMessage() const {
    return m_statusMessage;
}

/**
 * @brief 设置Content-Type
 * 
 * 指定响应体的MIME类型。
 * 
 * @param contentType MIME类型字符串
 */
void HttpResponse::setContentType(const std::string& contentType) {
    m_contentType = contentType;
}

/**
 * @brief 获取Content-Type
 * 
 * @return std::string 当前Content-Type
 */
std::string HttpResponse::getContentType() const {
    return m_contentType;
}

/**
 * @brief 添加响应头部
 * 
 * 向响应头部集合中添加一个新的头部字段。
 * 
 * @param key 头部字段名
 * @param value 头部字段值
 */
void HttpResponse::addHeader(const std::string& key, const std::string& value) {
    m_headers[key] = value;
}

/**
 * @brief 获取指定头部字段
 * 
 * @param key 头部字段名
 * @return std::string 头部字段值
 */
std::string HttpResponse::getHeader(const std::string& key) const {
    auto it = m_headers.find(key);
    if (it != m_headers.end()) {
        return it->second;
    }
    return "";
}

/**
 * @brief 获取所有头部字段
 * 
 * @return std::map<std::string, std::string> 头部集合的副本
 */
std::map<std::string, std::string> HttpResponse::getHeaders() const {
    return m_headers;
}

/**
 * @brief 设置响应体（字符串形式）
 * 
 * 设置响应消息体内容。
 * 设置响应体会同时清空文件路径。
 * 
 * @param body 响应体内容
 */
void HttpResponse::setBody(const std::string& body) {
    m_body = body;
    m_filePath.clear();  // 清除文件路径
}

/**
 * @brief 设置响应体（原始数据形式）
 * 
 * 设置响应消息体的原始字节数据。
 * 
 * @param data 指向数据的指针
 * @param len 数据长度
 */
void HttpResponse::setBody(const char* data, size_t len) {
    m_body.assign(data, len);
    m_filePath.clear();  // 清除文件路径
}

/**
 * @brief 获取响应体
 * 
 * @return std::string 响应体内容
 */
std::string HttpResponse::getBody() const {
    return m_body;
}

/**
 * @brief 获取响应体大小
 * 
 * 优先返回文件大小（如果设置了文件路径），
 * 否则返回内存中响应体的大小。
 * 
 * @return size_t 响应体字节数
 */
size_t HttpResponse::getBodySize() const {
    if (!m_filePath.empty()) {
        struct stat st;
        if (stat(m_filePath.c_str(), &st) == 0) {
            return st.st_size;
        }
    }
    return m_body.size();
}

/**
 * @brief 设置待发送的文件路径
 * 
 * 指定响应需要发送的静态文件。
 * 设置文件路径后，会自动根据文件扩展名设置Content-Type。
 * 
 * @param filepath 要发送的文件路径
 */
void HttpResponse::setFilePath(const std::string& filepath) {
    m_filePath = filepath;
    m_contentType = getContentTypeByExtension(filepath);  // 自动设置Content-Type
}

/**
 * @brief 获取文件路径
 * 
 * @return std::string 文件路径
 */
std::string HttpResponse::getFilePath() const {
    return m_filePath;
}

/**
 * @brief 转换为HTTP字符串格式
 * 
 * 将HttpResponse对象序列化为标准的HTTP响应字符串格式。
 * 
 * HTTP响应格式：
 * @code
 * HTTP/1.1 200 OK\r\n
 * Content-Type: text/html\r\n
 * Server: CppHttpServer/1.0\r\n
 * Content-Length: 1234\r\n
 * Connection: Close\r\n
 * \r\n
 * [响应体数据]
 * @endcode
 * 
 * @return std::string HTTP响应字符串
 */
std::string HttpResponse::toString() const {
    std::string result;

    // 1. 状态行：HTTP/1.1 <状态码> <状态消息>
    // 将枚举类型转换为整数后再转换为字符串
    result = "HTTP/1.1 " + std::to_string(static_cast<int>(m_statusCode)) + " " + m_statusMessage + "\r\n";

    // 2. Content-Type头部
    if (!m_contentType.empty()) {
        result += "Content-Type: " + m_contentType + "\r\n";
    }

    // 3. Server头部
    result += "Server: CppHttpServer/1.0\r\n";

    // 4. 其他自定义头部（Connection 统一在后面输出，避免重复）
    for (const auto& header : m_headers) {
        if (header.first == "Connection") {
            continue;
        }
        result += header.first + ": " + header.second + "\r\n";
    }

    // 5. Content-Length头部（无论响应体是在内存中还是文件中）
    // 必须始终添加 Content-Length，即使文件不存在也能让客户端知道响应体大小
    size_t bodySize = getBodySize();
    if (bodySize > 0 || !m_filePath.empty()) {
        result += "Content-Length: " + std::to_string(bodySize) + "\r\n";
    }

    // 6. Connection头部：优先使用已设置值，否则默认Close
    std::string connectionValue = getHeader("Connection");
    if (connectionValue.empty()) {
        connectionValue = "Close";
    }
    result += "Connection: " + connectionValue + "\r\n";

    // 7. 空行，标志头部结束
    result += "\r\n";

    // 注意：实际的响应体数据（文件内容）不在这里包含
    // 由HttpServer在发送响应后再单独读取并发送文件内容

    return result;
}

/**
 * @brief 构建HTTP响应头字符串（带指定的Content-Length）
 *
 * 与toString()类似，但使用传入的contentLength参数而不是getBodySize()。
 * 确保Content-Length与实际发送内容一致。
 *
 * @param contentLength 实际响应体大小（字节）
 * @return std::string HTTP响应头字符串
 */
std::string HttpResponse::buildHeaderString(size_t contentLength) const {
    std::string result;

    // 1. 状态行
    result = "HTTP/1.1 " + std::to_string(static_cast<int>(m_statusCode)) + " " + m_statusMessage + "\r\n";

    // 2. Content-Type头部
    if (!m_contentType.empty()) {
        result += "Content-Type: " + m_contentType + "\r\n";
    }

    // 3. Server头部
    result += "Server: CppHttpServer/1.0\r\n";

    // 4. 其他自定义头部（Connection 统一在后面输出，避免重复）
    for (const auto& header : m_headers) {
        if (header.first == "Connection") {
            continue;
        }
        result += header.first + ": " + header.second + "\r\n";
    }

    // 5. Content-Length头部（使用传入的参数）
    if (contentLength > 0) {
        result += "Content-Length: " + std::to_string(contentLength) + "\r\n";
    }

    // 6. Connection头部：优先使用已设置值，否则默认Close
    std::string connectionValue = getHeader("Connection");
    if (connectionValue.empty()) {
        connectionValue = "Close";
    }
    result += "Connection: " + connectionValue + "\r\n";

    // 7. 空行
    result += "\r\n";

    return result;
}

/**
 * @brief 将状态码转换为字符串形式
 * 
 * 静态方法，将状态码枚举转换为标准的状态消息文本。
 * 
 * @param code 状态码枚举值
 * @return std::string 状态消息
 */
std::string HttpResponse::statusCodeToString(StatusCode code) {
    switch (code) {
        case STATUS_200_OK: return "OK";
        case STATUS_201_CREATED: return "Created";
        case STATUS_204_NO_CONTENT: return "No Content";
        case STATUS_206_PARTIAL_CONTENT: return "Partial Content";
        case STATUS_301_MOVED_PERMANENTLY: return "Moved Permanently";
        case STATUS_302_FOUND: return "Found";
        case STATUS_304_NOT_MODIFIED: return "Not Modified";
        case STATUS_400_BAD_REQUEST: return "Bad Request";
        case STATUS_401_UNAUTHORIZED: return "Unauthorized";
        case STATUS_403_FORBIDDEN: return "Forbidden";
        case STATUS_404_NOT_FOUND: return "Not Found";
        case STATUS_405_METHOD_NOT_ALLOWED: return "Method Not Allowed";
        case STATUS_408_REQUEST_TIMEOUT: return "Request Timeout";
        case STATUS_413_PAYLOAD_TOO_LARGE: return "Payload Too Large";
        case STATUS_414_URI_TOO_LONG: return "URI Too Long";
        case STATUS_416_RANGE_NOT_SATISFIABLE: return "Range Not Satisfiable";
        case STATUS_500_INTERNAL_SERVER_ERROR: return "Internal Server Error";
        case STATUS_501_NOT_IMPLEMENTED: return "Not Implemented";
        case STATUS_502_BAD_GATEWAY: return "Bad Gateway";
        case STATUS_503_SERVICE_UNAVAILABLE: return "Service Unavailable";
        case STATUS_505_HTTP_VERSION_NOT_SUPPORTED: return "HTTP Version Not Supported";
        default: return "Unknown";
    }
}

/**
 * @brief 根据文件扩展名获取Content-Type
 * 
 * 静态方法，根据文件扩展名自动判断MIME类型。
 * 支持多种常见的文件类型。
 * 
 * @param path 文件路径
 * @return std::string 对应的MIME类型
 */
std::string HttpResponse::getContentTypeByExtension(const std::string& path) {
    // 查找最后一个点号的位置
    size_t pos = path.find_last_of('.');
    if (pos == std::string::npos) {
        return "text/plain";  // 无扩展名，默认为纯文本
    }

    // 获取文件扩展名（小写比较）
    std::string ext = path.substr(pos);

    // 文本类型
    if (ext == ".html" || ext == ".htm") return "text/html";
    if (ext == ".css") return "text/css";
    if (ext == ".txt") return "text/plain";

    // JavaScript和JSON
    if (ext == ".js") return "application/javascript";
    if (ext == ".json") return "application/json";
    if (ext == ".xml") return "application/xml";

    // 图片类型
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".png") return "image/png";
    if (ext == ".gif") return "image/gif";
    if (ext == ".bmp") return "image/bmp";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".svg") return "image/svg+xml";

    // 字体类型
    if (ext == ".woff") return "font/woff";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".ttf") return "font/ttf";
    if (ext == ".eot") return "application/vnd.ms-fontobject";

    // 其他类型
    if (ext == ".pdf") return "application/pdf";
    if (ext == ".zip") return "application/zip";

    // 默认未知类型
    return "application/octet-stream";
}

/**
 * @brief 创建400 Bad Request响应
 * 
 * 静态工厂方法，用于快速创建请求语法错误响应。
 * 
 * @return HttpResponse 预配置的400错误响应对象
 */
HttpResponse HttpResponse::badRequest() {
    HttpResponse response;
    response.setStatusCode(STATUS_400_BAD_REQUEST);
    response.setContentType("text/html; charset=utf-8");
    response.setBody("<html><head><title>400 Bad Request</title></head>"
                     "<body><center><h1>400 Bad Request</h1></center></body></html>");
    return response;
}

/**
 * @brief 创建404 Not Found响应
 * 
 * 静态工厂方法，用于快速创建资源未找到响应。
 * 
 * @return HttpResponse 预配置的404错误响应对象
 */
HttpResponse HttpResponse::notFound() {
    HttpResponse response;
    response.setStatusCode(STATUS_404_NOT_FOUND);
    response.setContentType("text/html; charset=utf-8");
    response.setBody("<html><head><title>404 Not Found</title></head>"
                     "<body><center><h1>404 Not Found</h1></center></body></html>");
    return response;
}

/**
 * @brief 创建405 Method Not Allowed响应
 * 
 * 静态工厂方法，用于快速创建HTTP方法不支持响应。
 * 
 * @return HttpResponse 预配置的405错误响应对象
 */
HttpResponse HttpResponse::methodNotAllowed() {
    HttpResponse response;
    response.setStatusCode(STATUS_405_METHOD_NOT_ALLOWED);
    response.setContentType("text/html; charset=utf-8");
    response.setBody("<html><head><title>405 Method Not Allowed</title></head>"
                     "<body><center><h1>405 Method Not Allowed</h1></center></body></html>");
    return response;
}

/**
 * @brief 创建500 Internal Server Error响应
 * 
 * 静态工厂方法，用于快速创建服务器内部错误响应。
 * 
 * @return HttpResponse 预配置的500错误响应对象
 */
HttpResponse HttpResponse::internalServerError() {
    HttpResponse response;
    response.setStatusCode(STATUS_500_INTERNAL_SERVER_ERROR);
    response.setContentType("text/html; charset=utf-8");
    response.setBody("<html><head><title>500 Internal Server Error</title></head>"
                     "<body><center><h1>500 Internal Server Error</h1></center></body></html>");
    return response;
}

/**
 * @brief 创建501 Not Implemented响应
 * 
 * 静态工厂方法，用于快速创建功能未实现响应。
 * 用于处理服务器不支持的HTTP方法。
 * 
 * @return HttpResponse 预配置的501错误响应对象
 */
HttpResponse HttpResponse::notImplemented() {
    HttpResponse response;
    response.setStatusCode(STATUS_501_NOT_IMPLEMENTED);
    response.setContentType("text/html; charset=utf-8");
    response.setBody("<html><head><title>501 Not Implemented</title></head>"
                     "<body><center><h1>501 Not Implemented</h1></center></body></html>");
    return response;
}
