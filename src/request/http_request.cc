/**
 * @file http_request.cc
 * @brief HTTP请求类的实现文件
 * 
 * 实现HttpRequest类中声明的所有方法，
 * 包括请求方法设置、URL解析、头部管理等功能。
 */

#include "../request/http_request.h"

#include <algorithm>     
#include <cstring>       // C字符串处理

/**
 * @brief 构造函数
 * 
 * 初始化HttpRequest对象，将所有成员设置为默认值。
 */
HttpRequest::HttpRequest()
    : m_method(METHOD_UNKNOWN),  // 默认方法为未知
      m_clientPort(0) {          // 默认端口为0
}

/**
 * @brief 析构函数
 * 
 * 销毁HttpRequest对象。
 * 由于使用标准库容器，析构函数自动处理资源释放。
 */
HttpRequest::~HttpRequest() {
}

/**
 * @brief 设置请求方法
 * 
 * @param method HTTP方法枚举值
 */
void HttpRequest::setMethod(Method method) {
    m_method = method;
}

/**
 * @brief 通过字符串设置请求方法
 * 
 * 将字符串形式的HTTP方法名转换为内部枚举表示。
 * 
 * @param method HTTP方法名字符串
 */
void HttpRequest::setMethodString(const std::string& method) {
    m_method = stringToMethod(method);
}

/**
 * @brief 获取请求方法
 * 
 * @return Method 当前请求的HTTP方法枚举值
 */
HttpRequest::Method HttpRequest::getMethod() const {
    return m_method;
}

/**
 * @brief 设置请求URL
 * 
 * 设置完整的请求URL，同时自动解析路径部分。
 * 如果URL包含查询字符串（?），路径将只取?之前的部分。
 * 
 * @param url 完整的请求URL
 * 
 * @par 示例：
 * @code
 * setUrl("/index.html?id=123")  // m_url = "/index.html?id=123"
 *                             // m_path = "/index.html"
 * @endcode
 */
void HttpRequest::setUrl(const std::string& url) {
    m_url = url;
    
    // 查找查询字符串起始位置（?）
    size_t pos = url.find('?');
    if (pos != std::string::npos) {
        // 存在查询字符串，只取路径部分
        m_path = url.substr(0, pos);
    } else {
        // 没有查询字符串，整个URL就是路径
        m_path = url;
    }
}

/**
 * @brief 获取请求URL
 * 
 * @return std::string 完整的请求URL
 */
std::string HttpRequest::getUrl() const {
    return m_url;
}

/**
 * @brief 设置请求路径
 * 
 * @param path 请求路径
 */
void HttpRequest::setPath(const std::string& path) {
    m_path = path;
}

/**
 * @brief 获取请求路径
 * 
 * @return std::string 请求路径
 */
std::string HttpRequest::getPath() const {
    return m_path;
}

/**
 * @brief 添加HTTP头部
 * 
 * 向头部集合中添加一个头部字段。
 * 如果已存在同名的key，则会覆盖原有值。
 * 
 * @param key 头部字段名
 * @param value 头部字段值
 */
void HttpRequest::addHeader(const std::string& key, const std::string& value) {
    m_headers[key] = value;
}

/**
 * @brief 获取指定头部字段
 * 
 * @param key 头部字段名
 * @return std::string 头部字段值，如果不存在返回空字符串
 */
std::string HttpRequest::getHeader(const std::string& key) const {
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
std::map<std::string, std::string> HttpRequest::getHeaders() const {
    return m_headers;
}

/**
 * @brief 设置请求体
 * 
 * @param body 请求体内容
 */
void HttpRequest::setBody(const std::string& body) {
    m_body = body;
}

/**
 * @brief 获取请求体
 * 
 * @return std::string 请求体内容
 */
std::string HttpRequest::getBody() const {
    return m_body;
}

/**
 * @brief 设置客户端IP地址
 * 
 * @param ip 客户端IP地址字符串
 */
void HttpRequest::setClientIp(const std::string& ip) {
    m_clientIp = ip;
}

/**
 * @brief 获取客户端IP地址
 * 
 * @return std::string 客户端IP地址
 */
std::string HttpRequest::getClientIp() const {
    return m_clientIp;
}

/**
 * @brief 设置客户端端口
 * 
 * @param port 客户端端口号
 */
void HttpRequest::setClientPort(int port) {
    m_clientPort = port;
}

/**
 * @brief 获取客户端端口
 * 
 * @return int 客户端端口号
 */
int HttpRequest::getClientPort() const {
    return m_clientPort;
}

/**
 * @brief 清空请求
 * 
 * 重置所有成员变量到初始状态。
 */
void HttpRequest::clear() {
    m_method = METHOD_UNKNOWN;
    m_url.clear();
    m_path.clear();
    m_headers.clear();
    m_body.clear();
    m_clientIp.clear();
    m_clientPort = 0;
}

/**
 * @brief 将字符串转换为HTTP方法枚举
 * 
 * 静态方法，支持不区分大小写的比较。
 * 
 * @param method HTTP方法名字符串
 * @return Method 对应的枚举值
 */
HttpRequest::Method HttpRequest::stringToMethod(const std::string& method) {
    // 使用strcasecmp进行不区分大小写的比较
    if (strcasecmp(method.c_str(), "GET") == 0) return METHOD_GET;
    if (strcasecmp(method.c_str(), "POST") == 0) return METHOD_POST;
    if (strcasecmp(method.c_str(), "PUT") == 0) return METHOD_PUT;
    if (strcasecmp(method.c_str(), "DELETE") == 0) return METHOD_DELETE;
    if (strcasecmp(method.c_str(), "HEAD") == 0) return METHOD_HEAD;
    return METHOD_UNKNOWN;
}

/**
 * @brief 将HTTP方法枚举转换为字符串
 * 
 * 静态方法，将枚举值转换为可读的方法名字符串。
 * 
 * @param method HTTP方法枚举值
 * @return std::string 方法名字符串
 */
std::string HttpRequest::methodToString(Method method) {
    switch (method) {
        case METHOD_GET:    return "GET";
        case METHOD_POST:   return "POST";
        case METHOD_PUT:    return "PUT";
        case METHOD_DELETE: return "DELETE";
        case METHOD_HEAD:   return "HEAD";
        default:            return "UNKNOWN";
    }
}

/**
 * @brief URL解码函数
 * 
 * 将URL编码的字符串解码为原始字符串。
 * 支持 %XX 格式的编码（如 %20 转换为空格）。
 * 
 * @param encoded URL编码的字符串
 * @return std::string 解码后的字符串
 */
std::string HttpRequest::urlDecode(const std::string& encoded) {
    std::string decoded;
    decoded.reserve(encoded.length());
    
    for (size_t i = 0; i < encoded.length(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.length()) {
            // 解析 %XX 格式的十六进制编码
            int hex1 = encoded[i + 1];
            int hex2 = encoded[i + 2];
            
            // 将十六进制字符转换为数值
            int value = 0;
            if (hex1 >= '0' && hex1 <= '9') value = (hex1 - '0') << 4;
            else if (hex1 >= 'A' && hex1 <= 'F') value = (hex1 - 'A' + 10) << 4;
            else if (hex1 >= 'a' && hex1 <= 'f') value = (hex1 - 'a' + 10) << 4;
            
            if (hex2 >= '0' && hex2 <= '9') value |= (hex2 - '0');
            else if (hex2 >= 'A' && hex2 <= 'F') value |= (hex2 - 'A' + 10);
            else if (hex2 >= 'a' && hex2 <= 'f') value |= (hex2 - 'a' + 10);
            
            decoded += static_cast<char>(value);
            i += 2;  // 跳过接下来的两个字符
        } else if (encoded[i] == '+') {
            // URL编码中 + 表示空格
            decoded += ' ';
        } else {
            decoded += encoded[i];
        }
    }
    
    return decoded;
}

/**
 * @brief 解析URL编码的查询参数
 * 
 * 从URL的查询字符串中解析出键值对参数。
 * 格式: ?key1=value1&key2=value2
 * 
 * @return std::map<std::string, std::string> 参数键值对
 */
std::map<std::string, std::string> HttpRequest::parseQueryParams() const {
    std::map<std::string, std::string> params;
    
    // 查找查询字符串起始位置
    size_t queryPos = m_url.find('?');
    if (queryPos == std::string::npos) {
        return params;  // 没有查询字符串
    }
    
    // 提取查询字符串部分（去掉?）
    std::string queryString = m_url.substr(queryPos + 1);
    
    // 解析键值对
    size_t start = 0;
    while (start < queryString.length()) {
        // 查找 & 分隔符
        size_t ampPos = queryString.find('&', start);
        std::string pair;
        
        if (ampPos == std::string::npos) {
            pair = queryString.substr(start);
            start = queryString.length();
        } else {
            pair = queryString.substr(start, ampPos - start);
            start = ampPos + 1;
        }
        
        // 解析 key=value
        size_t eqPos = pair.find('=');
        if (eqPos != std::string::npos) {
            std::string key = urlDecode(pair.substr(0, eqPos));
            std::string value = urlDecode(pair.substr(eqPos + 1));
            params[key] = value;
        } else if (!pair.empty()) {
            // 只有key没有value的情况
            params[urlDecode(pair)] = "";
        }
    }
    
    return params;
}

/**
 * @brief 解析POST请求体中的表单数据
 * 
 * 根据Content-Type解析请求体中的表单数据。
 * 支持 application/x-www-form-urlencoded 格式。
 * 
 * @return std::map<std::string, std::string> 表单数据键值对
 */
std::map<std::string, std::string> HttpRequest::parseFormData() const {
    std::map<std::string, std::string> formData;
    
    // 检查Content-Type是否为表单类型
    std::string contentType = getContentType();
    if (contentType.find("application/x-www-form-urlencoded") == std::string::npos) {
        return formData;  // 不是URL编码的表单数据
    }
    
    // 请求体格式与查询字符串相同: key1=value1&key2=value2
    size_t start = 0;
    while (start < m_body.length()) {
        // 查找 & 分隔符
        size_t ampPos = m_body.find('&', start);
        std::string pair;
        
        if (ampPos == std::string::npos) {
            pair = m_body.substr(start);
            start = m_body.length();
        } else {
            pair = m_body.substr(start, ampPos - start);
            start = ampPos + 1;
        }
        
        // 解析 key=value
        size_t eqPos = pair.find('=');
        if (eqPos != std::string::npos) {
            std::string key = urlDecode(pair.substr(0, eqPos));
            std::string value = urlDecode(pair.substr(eqPos + 1));
            formData[key] = value;
        } else if (!pair.empty()) {
            // 只有key没有value的情况
            formData[urlDecode(pair)] = "";
        }
    }
    
    return formData;
}

/**
 * @brief 获取Content-Type头部值
 * 
 * @return std::string Content-Type值，如果不存在返回空字符串
 */
std::string HttpRequest::getContentType() const {
    return getHeader("Content-Type");
}
