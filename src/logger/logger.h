/**
 * @file logger.h
 * @brief 结构化日志系统头文件
 *
 * 提供分级日志记录功能，支持访问日志和错误日志分离输出。
 * 支持控制台和文件同时输出，可配置日志级别。
 */

#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <fstream>
#include <mutex>
#include <memory>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <iostream>

/**
 * @brief 日志级别枚举
 *
 * 定义日志的严重程度级别，从DEBUG到ERROR递增
 */
enum class LogLevel {
    DEBUG = 0,  /**< 调试信息，最详细 */
    INFO = 1,   /**< 一般信息 */
    WARN = 2,   /**< 警告信息 */
    ERROR = 3   /**< 错误信息 */
};

/**
 * @brief 日志系统类
 *
 * 单例模式实现的线程安全日志系统。
 * 支持同时输出到控制台和文件，支持访问日志和错误日志分离。
 *
 * 使用示例：
 * @code
 * Logger::getInstance().info("Server started");
 * Logger::getInstance().error("File not found: {}", filename);
 * @endcode
 */
class Logger {
public:
    /**
     * @brief 获取Logger单例实例
     *
     * @return Logger& 日志器实例引用
     */
    static Logger& getInstance();

    /**
     * @brief 初始化日志系统
     *
     * @param accessLogPath 访问日志文件路径
     * @param errorLogPath 错误日志文件路径
     * @param minLevel 最低日志级别，低于此级别的日志不会记录
     * @param consoleOutput 是否同时输出到控制台
     * @return bool 初始化成功返回true
     */
    bool init(const std::string& accessLogPath = "./logs/access.log",
              const std::string& errorLogPath = "./logs/error.log",
              LogLevel minLevel = LogLevel::INFO,
              bool consoleOutput = true);

    /**
     * @brief 关闭日志系统
     *
     * 关闭所有日志文件句柄，释放资源
     */
    void shutdown();

    /**
     * @brief 设置最低日志级别
     *
     * @param level 最低日志级别
     */
    void setMinLevel(LogLevel level);

    /**
     * @brief 设置是否输出到控制台
     *
     * @param enabled true输出，false关闭
     */
    void setConsoleOutput(bool enabled);

    /**
     * @brief 记录DEBUG级别日志
     *
     * @param message 日志消息
     */
    void debug(const std::string& message);

    /**
     * @brief 记录INFO级别日志
     *
     * @param message 日志消息
     */
    void info(const std::string& message);

    /**
     * @brief 记录WARN级别日志
     *
     * @param message 日志消息
     */
    void warn(const std::string& message);

    /**
     * @brief 记录ERROR级别日志
     *
     * @param message 日志消息
     */
    void error(const std::string& message);

    /**
     * @brief 记录访问日志
     *
     * 专门用于记录HTTP访问信息，格式统一
     *
     * @param clientIp 客户端IP地址
     * @param method HTTP方法
     * @param url 请求URL
     * @param statusCode HTTP状态码
     * @param responseSize 响应体大小（字节）
     * @param durationMs 处理耗时（毫秒）
     */
    void access(const std::string& clientIp,
                const std::string& method,
                const std::string& url,
                int statusCode,
                size_t responseSize,
                double durationMs);

    /**
     * @brief 将日志级别转换为字符串
     *
     * @param level 日志级别
     * @return std::string 级别字符串（DEBUG/INFO/WARN/ERROR）
     */
    static std::string levelToString(LogLevel level);

private:
    /**
     * @brief 私有构造函数（单例模式）
     */
    Logger() = default;

    /**
     * @brief 析构函数
     */
    ~Logger();

    /**
     * @brief 禁止拷贝构造
     */
    Logger(const Logger&) = delete;

    /**
     * @brief 禁止赋值操作
     */
    Logger& operator=(const Logger&) = delete;

    /**
     * @brief 内部关闭函数（不加锁版本）
     *
     * 供init()在已持有锁的情况下调用
     */
    void shutdownInternal();

    /**
     * @brief 写入日志到文件
     *
     * @param level 日志级别
     * @param message 日志消息
     * @param isError 是否为错误日志（写入错误日志文件）
     */
    void writeLog(LogLevel level, const std::string& message, bool isError = false);

    /**
     * @brief 获取当前时间字符串
     *
     * @return std::string 格式化的时间字符串（YYYY-MM-DD HH:MM:SS）
     */
    static std::string getCurrentTime();

    /**
     * @brief 确保日志目录存在
     *
     * @param filePath 日志文件路径
     * @return bool 成功返回true
     */
    static bool ensureDirectoryExists(const std::string& filePath);

    // 成员变量
    std::ofstream m_accessLogFile;   /**< 访问日志文件流 */
    std::ofstream m_errorLogFile;    /**< 错误日志文件流 */
    std::mutex m_mutex;              /**< 线程安全互斥锁 */
    LogLevel m_minLevel = LogLevel::INFO;  /**< 最低日志级别 */
    bool m_consoleOutput = true;     /**< 是否输出到控制台 */
    bool m_initialized = false;      /**< 是否已初始化 */
};

/**
 * @brief 便捷宏定义，用于快速记录日志
 *
 * 使用示例：
 * @code
 * LOG_INFO("Server started on port {}", 8080);
 * LOG_ERROR("Failed to open file: {}", filename);
 * @endcode
 */
#define LOG_DEBUG(msg) Logger::getInstance().debug(msg)
#define LOG_INFO(msg) Logger::getInstance().info(msg)
#define LOG_WARN(msg) Logger::getInstance().warn(msg)
#define LOG_ERROR(msg) Logger::getInstance().error(msg)

/**
 * @brief 访问日志便捷宏
 */
#define LOG_ACCESS(ip, method, url, status, size, duration) \
    Logger::getInstance().access(ip, method, url, status, size, duration)

#endif // LOGGER_H
