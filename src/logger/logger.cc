/**
 * @file logger.cc
 * @brief 结构化日志系统实现文件
 *
 * 实现Logger类的所有方法，包括日志记录、文件操作等功能。
 */

#include "logger.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <libgen.h>
#include <cstring>
#include <cerrno>

/**
 * @brief 获取Logger单例实例
 *
 * 使用局部静态变量实现线程安全的单例模式（C++11及以上）
 *
 * @return Logger& 日志器实例引用
 */
Logger& Logger::getInstance() {
    static Logger instance;
    return instance;
}

/**
 * @brief 析构函数
 *
 * 确保在对象销毁时关闭所有日志文件
 */
Logger::~Logger() {
    shutdown();
}

/**
 * @brief 初始化日志系统
 *
 * 创建日志目录，打开日志文件，设置日志级别
 *
 * @param accessLogPath 访问日志文件路径
 * @param errorLogPath 错误日志文件路径
 * @param minLevel 最低日志级别
 * @param consoleOutput 是否同时输出到控制台
 * @return bool 初始化成功返回true
 */
bool Logger::init(const std::string& accessLogPath,
                  const std::string& errorLogPath,
                  LogLevel minLevel,
                  bool consoleOutput) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 如果已经初始化，先关闭
    if (m_initialized) {
        shutdownInternal();
    }

    m_minLevel = minLevel;
    m_consoleOutput = consoleOutput;

    // 创建日志目录
    if (!ensureDirectoryExists(accessLogPath)) {
        std::cerr << "Failed to create directory for: " << accessLogPath << std::endl;
        return false;
    }

    if (!ensureDirectoryExists(errorLogPath)) {
        std::cerr << "Failed to create directory for: " << errorLogPath << std::endl;
        return false;
    }

    // 打开访问日志文件（追加模式）
    m_accessLogFile.open(accessLogPath, std::ios::app);
    if (!m_accessLogFile.is_open()) {
        std::cerr << "Failed to open access log file: " << accessLogPath << std::endl;
        return false;
    }

    // 打开错误日志文件（追加模式）
    m_errorLogFile.open(errorLogPath, std::ios::app);
    if (!m_errorLogFile.is_open()) {
        std::cerr << "Failed to open error log file: " << errorLogPath << std::endl;
        m_accessLogFile.close();
        return false;
    }

    m_initialized = true;

    // 直接写入启动日志（不调用writeLog避免死锁）
    // 性能优化：使用 "\n" 替代 std::endl，删除 flush() 以利用内核缓冲区
    std::string timeStr = getCurrentTime();
    std::string logLine = "[" + timeStr + "] [INFO] Logger initialized";
    m_accessLogFile << logLine << "\n";
    if (m_consoleOutput) {
        std::cout << logLine << std::endl;
    }

    logLine = "[" + timeStr + "] [INFO] Access log: " + accessLogPath;
    m_accessLogFile << logLine << "\n";
    if (m_consoleOutput) {
        std::cout << logLine << std::endl;
    }

    logLine = "[" + timeStr + "] [INFO] Error log: " + errorLogPath;
    m_accessLogFile << logLine << "\n";
    if (m_consoleOutput) {
        std::cout << logLine << std::endl;
    }

    // 后台启动时需要立即可见启动日志，避免因缓冲造成"看不到启动记录"
    m_accessLogFile.flush(); 

    return true;
}

/**
 * @brief 内部关闭函数（不加锁版本）
 *
 * 供init()在已持有锁的情况下调用
 */
void Logger::shutdownInternal() {
    if (!m_initialized) {
        return;
    }

    if (m_accessLogFile.is_open()) {
        m_accessLogFile.close();
    }

    if (m_errorLogFile.is_open()) {
        m_errorLogFile.close();
    }

    m_initialized = false;
}

/**
 * @brief 关闭日志系统
 *
 * 关闭所有日志文件句柄，释放资源
 */
void Logger::shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    shutdownInternal();
}

/**
 * @brief 设置最低日志级别
 *
 * @param level 最低日志级别
 */
void Logger::setMinLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_minLevel = level;
}

/**
 * @brief 设置是否输出到控制台
 *
 * @param enabled true输出，false关闭
 */
void Logger::setConsoleOutput(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_consoleOutput = enabled;
}

/**
 * @brief 记录DEBUG级别日志
 *
 * @param message 日志消息
 */
void Logger::debug(const std::string& message) {
    writeLog(LogLevel::DEBUG, message, false);
}

/**
 * @brief 记录INFO级别日志
 *
 * @param message 日志消息
 */
void Logger::info(const std::string& message) {
    writeLog(LogLevel::INFO, message, false);
}

/**
 * @brief 记录WARN级别日志
 *
 * @param message 日志消息
 */
void Logger::warn(const std::string& message) {
    writeLog(LogLevel::WARN, message, true);
}

/**
 * @brief 记录ERROR级别日志
 *
 * @param message 日志消息
 */
void Logger::error(const std::string& message) {
    writeLog(LogLevel::ERROR, message, true);
}

/**
 * @brief 记录访问日志
 *
 * 专门用于记录HTTP访问信息，格式统一为：
 * [时间] [INFO] 客户端IP 方法 URL 状态码 响应大小 处理时间
 *
 * @param clientIp 客户端IP地址
 * @param method HTTP方法
 * @param url 请求URL
 * @param statusCode HTTP状态码
 * @param responseSize 响应体大小（字节）
 * @param durationMs 处理耗时（毫秒）
 */
void Logger::access(const std::string& clientIp,
                    const std::string& method,
                    const std::string& url,
                    int statusCode,
                    size_t responseSize,
                    double durationMs) {
    // 构建访问日志消息
    std::ostringstream oss;
    oss << clientIp << " " << method << " " << url << " "
        << statusCode << " " << responseSize << "B "
        << std::fixed << std::setprecision(3) << durationMs << "ms";

    std::lock_guard<std::mutex> lock(m_mutex);

    // 检查日志级别
    if (m_minLevel > LogLevel::INFO) {
        return;
    }

    std::string timeStr = getCurrentTime();
    std::string logLine = "[" + timeStr + "] [INFO] " + oss.str();

    // 写入访问日志文件
    // 性能优化：使用 "\n" 替代 std::endl，删除 flush() 以利用内核缓冲区
    if (m_accessLogFile.is_open()) {
        m_accessLogFile << logLine << "\n";
    }

    // 同时输出到控制台
    if (m_consoleOutput) {
        std::cout << logLine << "\n";
    }
}

/**
 * @brief 将日志级别转换为字符串
 *
 * @param level 日志级别
 * @return std::string 级别字符串
 */
std::string Logger::levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
        default:              return "UNKNOWN";
    }
}

/**
 * @brief 写入日志到文件
 *
 * @param level 日志级别
 * @param message 日志消息
 * @param isError 是否为错误日志（写入错误日志文件）
 */
void Logger::writeLog(LogLevel level, const std::string& message, bool isError) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 检查日志级别
    if (level < m_minLevel) {
        return;
    }

    // 检查是否已初始化
    if (!m_initialized) {
        // 未初始化时，输出到控制台
        std::string timeStr = getCurrentTime();
        std::string levelStr = levelToString(level);
        std::cout << "[" << timeStr << "] [" << levelStr << "] " << message << std::endl;
        return;
    }

    std::string timeStr = getCurrentTime();
    std::string levelStr = levelToString(level);
    std::string logLine = "[" + timeStr + "] [" + levelStr + "] " + message;

    // 写入访问日志文件（所有级别）
    // 性能优化：使用 "\n" 替代 std::endl，删除 flush() 以利用内核缓冲区
    if (m_accessLogFile.is_open()) {
        m_accessLogFile << logLine << "\n";
    }

    // 错误日志同时写入错误日志文件
    if (isError && m_errorLogFile.is_open()) {
        m_errorLogFile << logLine << "\n";
    }

    // 同时输出到控制台
    if (m_consoleOutput) {
        if (level >= LogLevel::ERROR) {
            std::cerr << logLine << "\n";
        } else {
            std::cout << logLine << "\n";
        }
    }
}

/**
 * @brief 获取当前时间字符串
 *
 * @return std::string 格式化的时间字符串（YYYY-MM-DD HH:MM:SS）
 */
std::string Logger::getCurrentTime() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);

    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time_t_now), "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

/**
 * @brief 确保日志目录存在
 *
 * 从文件路径中提取目录部分，递归创建所有不存在的目录
 *
 * @param filePath 日志文件路径
 * @return bool 成功返回true
 */
bool Logger::ensureDirectoryExists(const std::string& filePath) {
    // 找到最后一个斜杠的位置
    size_t lastSlash = filePath.find_last_of("/\\");
    if (lastSlash == std::string::npos) {
        // 没有目录部分，当前目录一定存在
        return true;
    }

    std::string dirPath = filePath.substr(0, lastSlash);

    // 检查目录是否已存在
    struct stat st;
    if (stat(dirPath.c_str(), &st) == 0) {
        return true;
    }

    // 递归创建父目录
    size_t pos = 0;
    while (pos < dirPath.length()) {
        // 找到下一个路径分隔符
        pos = dirPath.find('/', pos + 1);
        if (pos == std::string::npos) {
            pos = dirPath.length();
        }

        std::string parentPath = dirPath.substr(0, pos);
        if (parentPath.empty()) {
            continue;
        }

        // 检查父目录是否存在
        if (stat(parentPath.c_str(), &st) != 0) {
            // 父目录不存在，创建它
            if (mkdir(parentPath.c_str(), 0755) != 0) {
                if (errno != EEXIST) {
                    return false;
                }
            }
        }
    }

    return true;
}
