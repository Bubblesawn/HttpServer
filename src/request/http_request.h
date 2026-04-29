/**
 * @file http_request.h
 * @brief HTTP请求类的头文件定义
 *
 * 本文件定义了HttpRequest类，用于表示HTTP请求的各个组成部分。
 * 包括请求方法、URL、路径、头部信息和请求体等。
 */

#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <map>    // 键值对容器
#include <memory> // 智能指针
#include <string> // 字符串

/**
 * @brief HTTP请求类
 *
 * 用于封装HTTP请求的完整信息，包括：
 * - HTTP方法（GET、POST、PUT、DELETE等）
 * - 请求URL和路径
 * - HTTP头部字段
 * - 请求体内容
 * - 客户端连接信息（IP、端口）
 *
 * 该类提供了完整的getter和setter方法，用于访问和修改请求的各个部分。
 *
 * @note 这是一个简单的HTTP/1.1请求解析器，支持常见的HTTP方法
 */
class HttpRequest {
public:
  /**
   * @brief HTTP请求方法枚举
   *
   * 定义服务器支持的HTTP请求方法。
   * 每个方法对应HTTP协议中的一种操作类型。
   */
  enum Method {
    METHOD_GET,     /**< GET方法：请求获取指定资源 */
    METHOD_OPTIONS, /**< OPTIONS方法：查询支持的通信选项 */
    METHOD_POST,    /**< POST方法：向指定资源提交数据 */
    METHOD_PATCH,   /**< PATCH方法：对资源进行部分修改 */
    METHOD_PUT,     /**< PUT方法：替换指定资源 */
    METHOD_DELETE,  /**< DELETE方法：删除指定资源 */
    METHOD_HEAD, /**< HEAD方法：获取资源头部信息（与GET相同但不返回响应体） */
    METHOD_UNKNOWN /**< 未知方法：用于不支持的方法 */
  };

  /**
   * @brief 构造函数
   *
   * 创建一个新的HttpRequest对象，初始化为默认值。
   */
  HttpRequest();

  /**
   * @brief 析构函数
   *
   * 销毁HttpRequest对象，释放相关资源。
   */
  ~HttpRequest();

  /**
   * @brief 设置请求方法
   *
   * @param method HttpRequest::Method类型的枚举值
   */
  void setMethod(Method method);

  /**
   * @brief 通过字符串设置请求方法
   *
   * 将字符串形式的HTTP方法名（如"GET"）转换为枚举值。
   *
   * @param method HTTP方法名字符串（如"GET"、"POST"）
   */
  void setMethodString(const std::string &method);

  /**
   * @brief 获取请求方法
   *
   * @return Method 当前请求的HTTP方法枚举值
   */
  Method getMethod() const;

  /**
   * @brief 设置请求URL
   *
   * 设置客户端请求的完整URL，包括路径和查询字符串。
   * 同时会自动解析URL中的路径部分。
   *
   * @param url 完整的请求URL
   */
  void setUrl(const std::string &url);

  /**
   * @brief 获取请求URL
   *
   * @return std::string 完整的请求URL（包括查询参数）
   */
  std::string getUrl() const;

  /**
   * @brief 设置请求路径
   *
   * 设置请求的资源路径，不包括查询字符串。
   *
   * @param path 请求路径（如"/index.html"）
   */
  void setPath(const std::string &path);

  /**
   * @brief 获取请求路径
   *
   * @return std::string 请求的资源路径
   */
  std::string getPath() const;

  /**
   * @brief 添加HTTP头部
   *
   * 向请求头部集合中添加一个新的头部字段。
   * 如果已存在同名的头部，则会覆盖原有值。
   *
   * @param key 头部字段名（如"Content-Type"）
   * @param value 头部字段值（如"application/json"）
   */
  void addHeader(const std::string &key, const std::string &value);

  /**
   * @brief 获取指定头部字段的值
   *
   * @param key 头部字段名
   * @return std::string 头部字段值，如果不存在返回空字符串
   */
  std::string getHeader(const std::string &key) const;

  /**
   * @brief 获取所有头部字段
   *
   * @return std::map<std::string, std::string> 包含所有头部字段的键值对
   */
  std::map<std::string, std::string> getHeaders() const;

  /**
   * @brief 设置请求体
   *
   * 设置HTTP请求的消息体，通常用于POST和PUT请求。
   *
   * @param body 请求体内容
   */
  void setBody(const std::string &body);

  /**
   * @brief 获取请求体
   *
   * @return std::string 请求体内容
   */
  std::string getBody() const;

  /**
   * @brief 设置客户端IP地址
   *
   * @param ip 客户端IP地址字符串
   */
  void setClientIp(const std::string &ip);

  /**
   * @brief 获取客户端IP地址
   *
   * @return std::string 客户端IP地址
   */
  std::string getClientIp() const;

  /**
   * @brief 设置客户端端口
   *
   * @param port 客户端端口号
   */
  void setClientPort(int port);

  /**
   * @brief 获取客户端端口
   *
   * @return int 客户端端口号
   */
  int getClientPort() const;

  /**
   * @brief 清空请求
   *
   * 重置HttpRequest对象的所有成员变量到初始状态。
   * 适用于请求对象复用场景。
   */
  void clear();

  /**
   * @brief 设置HTTP版本
   *
   * @param version HTTP版本字符串（如"HTTP/1.1"）
   */
  void setVersion(const std::string &version);

  /**
   * @brief 获取HTTP版本
   *
   * @return std::string HTTP版本字符串（如"HTTP/1.1"）
   */
  std::string getVersion() const;

  /**
   * @brief 将字符串转换为HTTP方法枚举
   *
   * 静态方法，不依赖于具体的HttpRequest实例。
   * 支持不区分大小写的比较。
   *
   * @param method HTTP方法名字符串
   * @return Method 对应的枚举值，如果是未知方法返回METHOD_UNKNOWN
   */
  static Method stringToMethod(const std::string &method);

  /**
   * @brief 将HTTP方法枚举转换为字符串
   *
   * 静态方法，不依赖于具体的HttpRequest实例。
   *
   * @param method HTTP方法枚举值
   * @return std::string 方法名字符串（如"GET"）
   */
  static std::string methodToString(Method method);

  /**
   * @brief 解析URL编码的查询参数
   *
   * 从URL的查询字符串中解析出键值对参数。
   * 支持 application/x-www-form-urlencoded 格式。
   *
   * @return std::map<std::string, std::string> 参数键值对
   */
  std::map<std::string, std::string> parseQueryParams() const;

  /**
   * @brief 解析请求体中的表单数据
   *
   * 仅在 Content-Type 为 application/x-www-form-urlencoded 时生效。
   *
   * @return std::map<std::string, std::string> 表单数据键值对
   */
  std::map<std::string, std::string> parseBodyParams() const;

  /**
   * @brief 解析POST请求体中的表单数据
   *
   * 根据Content-Type解析请求体中的表单数据。
   * 支持 application/x-www-form-urlencoded 格式。
   *
   * @return std::map<std::string, std::string> 表单数据键值对
   */
  std::map<std::string, std::string> parseFormData() const;

  /**
   * @brief 解析JSON请求体
   *
   * 仅支持顶层JSON对象，返回键值对映射。
   * 字符串、数字、布尔值和null会被转换为字符串形式。
   * 嵌套对象和数组会以原始JSON文本保留。
   *
   * @return std::map<std::string, std::string> JSON字段键值对
   */
  std::map<std::string, std::string> parseJsonBody() const;

  /**
   * @brief 设置路径参数
   *
   * 路由系统在匹配到路径参数后调用，用于让处理器读取诸如 /users/{id} 里的 id。
   *
   * @param pathParams 路径参数键值对
   */
  void setPathParams(const std::map<std::string, std::string> &pathParams);

  /**
   * @brief 获取所有路径参数
   *
   * @return std::map<std::string, std::string> 路径参数键值对
   */
  std::map<std::string, std::string> getPathParams() const;

  /**
   * @brief 获取指定路径参数
   *
   * @param key 参数名
   * @return std::string 参数值，不存在返回空字符串
   */
  std::string getPathParam(const std::string &key) const;

  /**
   * @brief 清空路径参数
   */
  void clearPathParams();

  /**
   * @brief 获取统一参数值
   *
   * 依次从查询参数、表单参数和JSON请求体中查找指定键。
   *
   * @param key 参数名
   * @return std::string 参数值，不存在则返回空字符串
   */
  std::string getParameter(const std::string &key) const;

  /**
   * @brief 获取Content-Type头部值
   *
   * @return std::string Content-Type值，如果不存在返回空字符串
   */
  std::string getContentType() const;

  /**
   * @brief URL解码函数
   *
   * 将URL编码的字符串解码为原始字符串。
   * 支持 %XX 格式的编码。
   *
   * @param encoded URL编码的字符串
   * @return std::string 解码后的字符串
   */
  static std::string urlDecode(const std::string &encoded);

private:
  /** HTTP请求方法 */
  Method m_method;

  /** 完整的请求URL */
  std::string m_url;

  /** 请求路径（不包括查询字符串） */
  std::string m_path;

  /** HTTP头部集合 */
  std::map<std::string, std::string> m_headers;

  /** 请求体内容 */
  std::string m_body;

  /** 路径参数 */
  std::map<std::string, std::string> m_pathParams;

  /** 客户端IP地址 */
  std::string m_clientIp;

  /** 客户端端口号 */
  int m_clientPort;

  /** HTTP版本 */
  std::string m_version;
};

#endif // HTTP_REQUEST_H
