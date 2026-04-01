/**
 * @file http_response.h
 * @brief HTTP响应类的头文件定义
 * 
 * 本文件定义了HttpResponse类，用于表示HTTP响应的各个组成部分。
 * 包括状态码、状态消息、响应头部、响应体等内容。
 */

#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <string>          // 字符串
#include <map>             // 键值对容器
#include <memory>          // 智能指针
#include <cstdint>         // 固定宽度整数类型

/**
 * @brief HTTP响应类
 * 
 * 用于封装HTTP响应的完整信息，包括：
 * - HTTP状态码（200、404、500等）
 * - 状态消息（OK、Not Found等）
 * - 响应头部字段
 * - 响应体内容
 * - 待发送的文件路径
 * 
 * 该类提供了创建常见错误响应的静态工厂方法，
 * 以及将响应对象序列化为HTTP格式字符串的功能。
 * 
 * @note 支持HTTP/1.1协议
 */
class HttpResponse {
public:
    /**
     * @brief HTTP状态码枚举
     * 
     * 定义常用的HTTP响应状态码。
     * 每个状态码都有特定的语义和用途。
     */
    enum StatusCode {
        // 2xx - 成功响应
        STATUS_200_OK = 200,                     /**< 请求成功 */
        STATUS_201_CREATED = 201,                /**< 资源创建成功 */
        STATUS_204_NO_CONTENT = 204,             /**< 请求成功，无返回内容 */
        STATUS_206_PARTIAL_CONTENT = 206,        /**< 部分内容（Range 请求） */

        // 3xx - 重定向
        STATUS_301_MOVED_PERMANENTLY = 301,      /**< 资源永久移动 */
        STATUS_302_FOUND = 302,                 /**< 资源临时移动 */
        STATUS_304_NOT_MODIFIED = 304,           /**< 资源未修改（使用缓存） */

        // 4xx - 客户端错误
        STATUS_400_BAD_REQUEST = 400,            /**< 请求语法错误 */
        STATUS_401_UNAUTHORIZED = 401,           /**< 需要身份认证 */
        STATUS_403_FORBIDDEN = 403,              /**< 服务器拒绝访问 */
        STATUS_404_NOT_FOUND = 404,              /**< 资源不存在 */
        STATUS_405_METHOD_NOT_ALLOWED = 405,    /**< 不支持的方法 */
        STATUS_408_REQUEST_TIMEOUT = 408,        /**< 请求超时 */
        STATUS_413_PAYLOAD_TOO_LARGE = 413,      /**< 请求体过大 */
        STATUS_414_URI_TOO_LONG = 414,           /**< 请求URI过长 */
        STATUS_416_RANGE_NOT_SATISFIABLE = 416,  /**< 请求范围无效 */

        // 5xx - 服务器错误
        STATUS_500_INTERNAL_SERVER_ERROR = 500, /**< 服务器内部错误 */
        STATUS_501_NOT_IMPLEMENTED = 501,        /**< 功能未实现 */
        STATUS_502_BAD_GATEWAY = 502,            /**< 网关错误 */
        STATUS_503_SERVICE_UNAVAILABLE = 503,    /**< 服务不可用 */
        STATUS_505_HTTP_VERSION_NOT_SUPPORTED = 505  /**< HTTP版本不支持 */
    };

    /**
     * @brief 构造函数
     * 
     * 创建一个新的HttpResponse对象，初始化为200 OK状态。
     */
    HttpResponse();

    /**
     * @brief 析构函数
     * 
     * 销毁HttpResponse对象，释放相关资源。
     */
    ~HttpResponse();

    /**
     * @brief 设置HTTP状态码
     * 
     * 设置响应的状态码，同时自动更新状态消息。
     * 
     * @param code StatusCode枚举值
     */
    void setStatusCode(StatusCode code);

    /**
     * @brief 获取HTTP状态码
     * 
     * @return StatusCode 当前状态码
     */
    StatusCode getStatusCode() const;

    /**
     * @brief 设置状态消息
     * 
     * 自定义状态消息文本。
     * 通常不需要手动调用，setStatusCode会自动设置标准消息。
     * 
     * @param message 状态消息（如"OK"、"Not Found"）
     */
    void setStatusMessage(const std::string& message);

    /**
     * @brief 获取状态消息
     * 
     * @return std::string 当前状态消息
     */
    std::string getStatusMessage() const;

    /**
     * @brief 设置Content-Type
     * 
     * 指定响应体的MIME类型。
     * 例如："text/html"、"application/json"、"image/png"等。
     * 
     * @param contentType MIME类型字符串
     */
    void setContentType(const std::string& contentType);

    /**
     * @brief 获取Content-Type
     * 
     * @return std::string 当前Content-Type值
     */
    std::string getContentType() const;

    /**
     * @brief 添加响应头部
     * 
     * 向响应头部集合中添加一个新的头部字段。
     * 如果已存在同名的头部，则会覆盖原有值。
     * 
     * @param key 头部字段名（如"Cache-Control"）
     * @param value 头部字段值（如"no-cache"）
     */
    void addHeader(const std::string& key, const std::string& value);

    /**
     * @brief 获取指定头部字段
     * 
     * @param key 头部字段名
     * @return std::string 头部字段值，如果不存在返回空字符串
     */
    std::string getHeader(const std::string& key) const;

    /**
     * @brief 获取所有头部字段
     * 
     * @return std::map<std::string, std::string> 包含所有头部字段的键值对
     */
    std::map<std::string, std::string> getHeaders() const;

    /**
     * @brief 设置响应体（字符串形式）
     * 
     * 设置响应消息体内容。
     * 设置响应体会同时清空文件路径。
     * 
     * @param body 响应体内容
     */
    void setBody(const std::string& body);

    /**
     * @brief 设置响应体（原始数据形式）
     * 
     * 设置响应消息体的原始字节数据。
     * 
     * @param data 指向数据的指针
     * @param len 数据长度
     */
    void setBody(const char* data, size_t len);

    /**
     * @brief 获取响应体
     * 
     * @return std::string 响应体内容
     */
    std::string getBody() const;

    /**
     * @brief 获取响应体大小
     * 
     * 如果设置了文件路径，返回文件大小；
     * 否则返回内存中响应体的大小。
     * 
     * @return size_t 响应体字节数
     */
    size_t getBodySize() const;

    /**
     * @brief 设置待发送的文件路径
     * 
     * 指定响应需要发送的静态文件。
     * 设置文件路径后，响应类会自动根据文件扩展名设置Content-Type。
     * 
     * @param filepath 要发送的文件路径
     */
    void setFilePath(const std::string& filepath);

    /**
     * @brief 获取文件路径
     * 
     * @return std::string 文件路径，如果未设置返回空字符串
     */
    std::string getFilePath() const;

    /**
     * @brief 转换为HTTP字符串格式
     * 
     * 将HttpResponse对象序列化为标准的HTTP响应字符串格式。
     * 包含状态行、所有头部字段和响应体（如果是内存数据）。
     * 
     * @return std::string HTTP响应字符串
     */
    std::string toString() const;

    /**
     * @brief 构建HTTP响应头字符串（带指定的Content-Length）
     *
     * 与toString()类似，但允许指定Content-Length值。
     * 用于缓存场景下，确保Content-Length与实际发送内容一致。
     *
     * @param contentLength 实际响应体大小（字节）
     * @return std::string HTTP响应头字符串（不包含响应体）
     */
    std::string buildHeaderString(size_t contentLength) const;

    /**
     * @brief 将状态码转换为字符串形式
     * 
     * 静态方法，将状态码枚举转换为可读的状态消息。
     * 
     * @param code 状态码枚举值
     * @return std::string 状态消息（如"OK"、"Not Found"）
     */
    static std::string statusCodeToString(StatusCode code);

    /**
     * @brief 转义JSON字符串
     * 
     * 将普通文本转义为可安全放入JSON字符串字面量中的内容。
     * 
     * @param text 原始文本
     * @return std::string JSON转义后的字符串内容（不含外层引号）
     */
    static std::string escapeJsonString(const std::string& text);

    /**
     * @brief 创建JSON响应
     * 
     * 自动设置JSON Content-Type，并写入指定JSON文本作为响应体。
     * 
     * @param code 状态码
     * @param jsonBody 已构造好的JSON文本
     * @return HttpResponse JSON响应对象
     */
    static HttpResponse jsonResponse(StatusCode code, const std::string& jsonBody);

    /**
     * @brief 创建JSON错误响应
     * 
     * 生成统一的JSON错误包裹格式。
     * 
     * @param code 状态码
     * @param message 错误消息
     * @return HttpResponse JSON错误响应对象
     */
    static HttpResponse jsonError(StatusCode code, const std::string& message);

    /**
     * @brief 根据文件扩展名获取Content-Type
     * 
     * 静态方法，根据文件扩展名自动判断MIME类型。
     * 支持HTML、CSS、JavaScript、图片、字体等多种类型。
     * 
     * @param path 文件路径
     * @return std::string 对应的MIME类型
     */
    static std::string getContentTypeByExtension(const std::string& path);

    //================== 静态工厂方法 ==================

    /**
     * @brief 创建400 Bad Request响应
     * 
     * 静态工厂方法，用于快速创建错误响应。
     * 
     * @return HttpResponse 预配置的400错误响应对象
     */
    static HttpResponse badRequest();

    /**
     * @brief 创建404 Not Found响应
     * 
     * 静态工厂方法，用于快速创建资源未找到响应。
     * 
     * @return HttpResponse 预配置的404错误响应对象
     */
    static HttpResponse notFound();

    /**
     * @brief 创建405 Method Not Allowed响应
     * 
     * 静态工厂方法，用于快速创建方法不支持响应。
     * 
     * @return HttpResponse 预配置的405错误响应对象
     */
    static HttpResponse methodNotAllowed();

    /**
     * @brief 创建405 Method Not Allowed响应
     * 
     * @param allowMethods Allow头部值，例如"GET, HEAD"
     * @return HttpResponse 预配置的405错误响应对象
     */
    static HttpResponse methodNotAllowed(const std::string& allowMethods);

    /**
     * @brief 创建500 Internal Server Error响应
     * 
     * 静态工厂方法，用于快速创建服务器内部错误响应。
     * 
     * @return HttpResponse 预配置的500错误响应对象
     */
    static HttpResponse internalServerError();

    /**
     * @brief 创建501 Not Implemented响应
     * 
     * 静态工厂方法，用于快速创建功能未实现响应。
     * 
     * @return HttpResponse 预配置的501错误响应对象
     */
    static HttpResponse notImplemented();

private:
    /** HTTP状态码 */
    StatusCode m_statusCode;

    /** HTTP状态消息 */
    std::string m_statusMessage;

    /** Content-Type */
    std::string m_contentType;

    /** 响应头部集合 */
    std::map<std::string, std::string> m_headers;

    /** 响应体内容 */
    std::string m_body;

    /** 待发送的文件路径 */
    std::string m_filePath;
};

#endif // HTTP_RESPONSE_H
