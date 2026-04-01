#include <iostream>
#include <csignal>
#include <cstdlib>
#include <getopt.h>
#include <fstream>
#include <sstream>
#include <string>
#include <filesystem>
#include "../server/http_server.h"
#include "../logger/logger.h"

namespace fs = std::filesystem;

/**
 * @brief 全局服务器实例指针
 * 
 * 用于在信号处理函数中访问服务器实例以便优雅地关闭服务器。
 * 由于信号处理函数不能使用类的成员函数，使用全局指针是必要的解决方案。
 */
static HttpServer* g_server = nullptr;
static volatile sig_atomic_t g_reloadRequested = 0;

/**
 * @brief 信号处理函数
 * 
 * 捕获 SIGINT（Ctrl+C）和 SIGTERM（终止信号）信号，
 * 执行服务器的优雅关闭操作。
 * 
 * @param signum 信号编号
 */
void signalHandler(int signum) {
    // 处理 SIGINT(用户按 Ctrl+C)  和 SIGTERM(用户输入kill) 信号
    if (signum == SIGINT || signum == SIGTERM) {
        LOG_INFO("Shutting down server...");
        if (g_server) {
            g_server->stop();
        }
        // 关闭日志系统
        Logger::getInstance().shutdown();
        exit(0);
    } else if (signum == SIGHUP) {
        g_reloadRequested = 1;
    }
}

/**
 * @brief 配置文件解析结构体
 * 
 * 用于存储从配置文件中读取的各项参数。
 */
struct Config {
    int port;              /**< 服务器监听端口 */
    int threadPoolSize;    /**< 线程池大小 */
    std::string docRoot;   /**< 文档根目录 */
    int debug;             /**< 调试模式 */
    int logToConsole;      /**< 日志是否输出到控制台 (1=是, 0=否) */
    int cacheEnabled;      /**< 是否启用文件缓存 */
    size_t cacheMaxSize;   /**< 缓存最大容量 */
    size_t cacheMaxFileSize; /**< 单文件最大缓存大小 */
    
    Config()
        : port(8080)
        , threadPoolSize(8)
        , docRoot("./html_docs")
        , debug(0)
        , logToConsole(0)
        , cacheEnabled(1)
        , cacheMaxSize(100 * 1024 * 1024)
        , cacheMaxFileSize(1 * 1024 * 1024) {}
};

LogLevel debugToLogLevel(int debug) {
    return debug != 0 ? LogLevel::DEBUG : LogLevel::INFO;
}

/**
 * @brief 解析配置文件
 * 
 * 读取并解析 HTTP 服务器的配置文件，支持以下选项：
 *   - port: 服务器监听端口
 *   - thread_pool_size: 线程池大小
 *   - doc_root: 文档根目录
 *   - debug: 调试模式
 *   - log_to_console: 日志是否输出到控制台
 * @param configFilePath 配置文件路径
 * @param config 输出参数，解析后的配置结果
 * @return bool 解析成功返回 true，失败返回 false
 */
bool parseConfigFile(const std::string& configFilePath, Config& config) {
    std::ifstream configFile(configFilePath);
    if (!configFile.is_open()) {
        std::cerr << "Error: Cannot open config file: " << configFilePath << std::endl;
        return false;
    }
    
    std::string line;
    while (std::getline(configFile, line)) {
        // 去除首尾空白字符
        size_t start = line.find_first_not_of(" \t\r\n");
        size_t end = line.find_last_not_of(" \t\r\n");
        
        // 跳过空行和注释行
        if (start == std::string::npos || line[start] == '#') {
            continue;
        }
        
        line = line.substr(start, end - start + 1);
        
        // 解析 key value 格式
        size_t spacePos = line.find(' ');
        if (spacePos == std::string::npos) {
            continue;
        }
        
        std::string key = line.substr(0, spacePos);
        std::string value = line.substr(spacePos + 1);
        
        // 去除 value 中的空白
        start = value.find_first_not_of(" \t");
        end = value.find_last_not_of(" \t");
        if (start != std::string::npos) {
            value = value.substr(start, end - start + 1);
        }
        
        if (key == "port") {
            try {
                config.port = std::stoi(value);
            } catch (...) {
                std::cerr << "Warning: invalid port value in config: " << value << std::endl;
            }
        } else if (key == "thread_pool_size") {
            try {
                config.threadPoolSize = std::stoi(value);
            } catch (...) {
                std::cerr << "Warning: invalid thread_pool_size value in config: " << value << std::endl;
            }
        } else if (key == "doc_root") {
            config.docRoot = value;
        } else if (key == "debug") {
            try {
                config.debug = std::stoi(value);
            } catch (...) {
                std::cerr << "Warning: invalid debug value in config: " << value << std::endl;
            }
        } else if (key == "log_to_console") {
            try {
                config.logToConsole = std::stoi(value);
            } catch (...) {
                std::cerr << "Warning: invalid log_to_console value in config: " << value << std::endl;
            }
        } else if (key == "cache_enabled") {
            try {
                config.cacheEnabled = std::stoi(value);
            } catch (...) {
                std::cerr << "Warning: invalid cache_enabled value in config: " << value << std::endl;
            }
        } else if (key == "cache_max_size") {
            try {
                config.cacheMaxSize = static_cast<size_t>(std::stoull(value));
            } catch (...) {
                std::cerr << "Warning: invalid cache_max_size value in config: " << value << std::endl;
            }
        } else if (key == "cache_max_file_size") {
            try {
                config.cacheMaxFileSize = static_cast<size_t>(std::stoull(value));
            } catch (...) {
                std::cerr << "Warning: invalid cache_max_file_size value in config: " << value << std::endl;
            }
        }
    }
    
    configFile.close();
    return true;
}

/**
 * @brief 在常见路径中定位配置文件
 *
 * 按顺序尝试：
 * 1. 传入路径（相对当前工作目录）
 * 2. 可执行文件目录下同名路径
 * 3. 可执行文件父目录下同名路径（常见于 build/ 场景）
 *
 * @param requestedPath 用户请求的配置路径
 * @param argv0 程序路径
 * @return std::string 可读取的配置文件路径；若都失败则返回原路径
 */
std::string resolveConfigPath(const std::string& requestedPath, const char* argv0) {
    fs::path requested(requestedPath);

    // 绝对路径直接检查
    if (requested.is_absolute() && fs::exists(requested)) {
        return requested.string();
    }

    // 当前工作目录
    if (fs::exists(requested)) {
        return requested.string();
    }

    fs::path exePath = fs::absolute(fs::path(argv0 ? argv0 : ""));
    fs::path exeDir = exePath.has_parent_path() ? exePath.parent_path() : fs::current_path();

    // 可执行文件目录
    fs::path candidate = exeDir / requested;
    if (fs::exists(candidate)) {
        return candidate.string();
    }

    // 可执行文件父目录（如从 build/ 启动，配置在项目根）
    candidate = exeDir.parent_path() / requested;
    if (fs::exists(candidate)) {
        return candidate.string();
    }

    // 当前工作目录父目录（常见：在 build/ 下运行，配置在 ../）
    candidate = fs::current_path().parent_path() / requested;
    if (fs::exists(candidate)) {
        return candidate.string();
    }
    return requestedPath;
}

/**
 * @brief 将相对路径转换为基于配置文件目录的绝对路径
 */
std::string resolvePathByConfigDir(const std::string& pathValue, const std::string& configFilePath) {
    fs::path value(pathValue);
    if (value.is_absolute()) {
        return value.string();
    }

    fs::path configDir = fs::path(configFilePath).parent_path();
    if (configDir.empty()) {
        configDir = fs::current_path();
    }

    return (configDir / value).lexically_normal().string();
}

/**
 * @brief 解析项目根目录
 *
 * 从可执行文件所在位置向上查找 CMakeLists.txt，确保无论从 build/ 还是项目根目录启动，
 * 都能定位到仓库根目录。
 */
fs::path resolveProjectRoot(const char* argv0) {
    fs::path exePath = fs::absolute(fs::path(argv0 ? argv0 : ""));
    fs::path currentDir = exePath.has_parent_path() ? exePath.parent_path() : fs::current_path();

    while (!currentDir.empty()) {
        if (fs::exists(currentDir / "CMakeLists.txt")) {
            return currentDir;
        }

        fs::path parentDir = currentDir.parent_path();
        if (parentDir == currentDir) {
            break;
        }
        currentDir = parentDir;
    }

    return fs::current_path();
}

/**
 * @brief 打印程序使用说明
 * 
 * 显示所有可用的命令行选项及其说明。
 * 
 * @param programName 程序名称（通常为 argv[0]）
 */
void printUsage(const char* programName) {
    std::cout << "Usage: " << programName << " [OPTIONS]\n"
              << "Options:\n"
              << "  -c, --config FILE      Configuration file (default: ./http_server.conf)\n"
              << "  -p, --port PORT        Server port (default: 8080)\n"
              << "  -d, --doc-root DIR     Document root directory (default: ./html_docs)\n"
              << "  -t, --threads NUM      Number of threads (default: 4)\n"
              << "  -h, --help             Show this help message\n"
              << "  -v, --version          Show version information\n";
}

/**
 * @brief 打印程序版本信息
 * 
 * 显示当前HTTP服务器的版本号。
 */
void printVersion() {
    std::cout << "CppHttpServer version 1.0.0\n";
}

void applyRuntimeConfig(HttpServer& server, const Config& config, const std::string& resolvedDocRoot) {
    server.setDocRoot(resolvedDocRoot);
    server.setCacheEnabled(config.cacheEnabled != 0);
    server.setCacheMaxSize(config.cacheMaxSize);
    server.setCacheMaxFileSize(config.cacheMaxFileSize);

    Logger::getInstance().setMinLevel(debugToLogLevel(config.debug));
    Logger::getInstance().setConsoleOutput(config.logToConsole != 0);
}

/**
 * @brief 主函数 - HTTP服务器程序入口
 * 
 * 解析命令行参数，初始化并启动HTTP服务器。
 * 支持通过命令行选项配置服务器端口、文档根目录和线程池大小。
 * 
 * @param argc 命令行参数个数
 * @param argv 命令行参数数组
 * @return int 程序返回码（0表示正常退出，非0表示错误）
 * 
 * 命令行选项说明：
 *   -p, --port:      指定服务器监听端口（默认8080）
 *   -d, --doc-root: 指定文档根目录（默认./html_docs）
 *   -t, --threads:  指定线程池线程数量（默认4）
 *   -h, --help:     显示帮助信息
 *   -v, --version:  显示版本信息
 * 
 * 示例用法：
 *   ./http_server -p 8080 -d /var/www/html -t 8
 *   ./http_server --port=9000 --doc-root=/home/user/www
 */
int main(int argc, char* argv[]) {
    /** 配置文件路径 */
    std::string configFile = "./http_server.conf";
    std::string resolvedConfigFile = configFile;
    
    /**
     * 第一步：先解析 -c 参数获取配置文件路径
     */
    static struct option configOption[] = {
        {"config", required_argument, 0, 'c'},
        {0, 0, 0, 0}
    };
    
    int opt;
    int optionIndex = 0;
    int oldOpterr = opterr;
    opterr = 0;  // 首轮仅识别 -c，忽略其他参数的错误提示
    while ((opt = getopt_long(argc, argv, "c:", configOption, &optionIndex)) != -1) {
        if (opt == 'c') {
            configFile = optarg;
        }
    }
    opterr = oldOpterr;
    
    /** 重置 optind 以便后续解析其他参数 */
    optind = 1;
    
    /**
     * 第二步：解析配置文件
     */
    Config config;
    resolvedConfigFile = resolveConfigPath(configFile, argv[0]);
    if (parseConfigFile(resolvedConfigFile, config)) {
        std::cout << "Loaded config from: " << resolvedConfigFile << std::endl;
    }
    
    /** 使用配置文件的值作为默认值 */
    int port = config.port;
    std::string docRoot = config.docRoot;
    int numThreads = config.threadPoolSize;

    // 若配置中是相对路径，则基于配置文件目录解析，避免在 build/ 启动时路径失效
    if (!resolvedConfigFile.empty()) {
        docRoot = resolvePathByConfigDir(docRoot, resolvedConfigFile);
    }
    
    /** 第三步：解析其他命令行参数（命令行参数优先级最高） */
    static struct option longOptions[] = {
        {"config", required_argument, 0, 'c'},
        {"port", required_argument, 0, 'p'},
        {"doc-root", required_argument, 0, 'd'},
        {"threads", required_argument, 0, 't'},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},
        {0, 0, 0, 0}
    };

    optionIndex = 0;
    int c;

    /**
     * 解析命令行参数循环
     * getopt_long 会依次处理每个选项：
     *   - 短选项（如 -p）
     *   - 长选项（如 --port）
     *   - 带参数的选项（如 -p 8080 或 --port=8080）
     * 
     * 返回值说明：
     *   - 字符: 成功解析一个选项
     *   - -1:   所有选项已解析完毕
     *   - '?':  遇到未知选项或缺少必需参数
     */
    while ((c = getopt_long(argc, argv, "c:p:d:t:hv", longOptions, &optionIndex)) != -1) {
        switch (c) {
            case 'c':
                /** 设置配置文件路径 */
                configFile = optarg;
                resolvedConfigFile = resolveConfigPath(configFile, argv[0]);
                break;
            case 'p':
                /** 解析端口参数 */
                port = std::atoi(optarg);
                /** 端口号有效性验证：必须在1-65535范围内 */
                if (port <= 0 || port > 65535) {
                    std::cerr << "Invalid port number: " << optarg << std::endl;
                    return 1;
                }
                break;
            case 'd':
                /** 设置文档根目录 */
                docRoot = optarg;
                break;
            case 't':
                /** 解析线程数量参数 */
                numThreads = std::atoi(optarg);
                /** 线程数必须大于0 */
                if (numThreads <= 0) {
                    std::cerr << "Invalid thread number: " << optarg << std::endl;
                    return 1;
                }
                break;
            case 'h':
                /** 显示帮助信息 */
                printUsage(argv[0]);
                return 0;
            case 'v':
                /** 显示版本信息 */
                printVersion();
                return 0;
            default:
                /** 未知选项，显示用法并退出 */
                printUsage(argv[0]);
                return 1;
        }
    }

    /**
     * @brief 设置标准输出/错误流为无缓冲模式
     * 
     * setvbuf() 用于控制缓冲行为：
     *   - _IONBF: 无缓冲模式（No Buffering）
     * 
     * 为什么要使用无缓冲：
     *   - 确保日志和消息立即输出到控制台
     *   - 避免在服务器日志中丢失重要信息
     *   - 便于调试和实时监控服务器状态
     */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /**
     * @brief 注册信号处理函数
     * 
     * 设置对 SIGINT 和 SIGTERM 信号的处理：
     *   - SIGINT (Ctrl+C): 用户主动中断服务器
     *   - SIGTERM: 系统发送的终止请求
     * 
     * 这样做可以确保服务器优雅关闭：
     *   - 停止接受新连接
     *   - 等待正在处理的请求完成
     *   - 释放资源（如关闭线程池、socket等）
     */
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGHUP, signalHandler);

    // 初始化日志系统
    // 根据配置文件中的 log_to_console 设置决定是否输出到控制台
    bool consoleOutput = (config.logToConsole != 0);
    fs::path executablePath = fs::absolute(fs::path(argv[0] ? argv[0] : ""));
    fs::path logDir = executablePath.has_parent_path() ? executablePath.parent_path() / "logs" : fs::current_path() / "logs";
    fs::create_directories(logDir);

    std::string accessLogPath = (logDir / "access.log").string();
    std::string errorLogPath = (logDir / "error.log").string();
    if (!Logger::getInstance().init(accessLogPath, errorLogPath, debugToLogLevel(config.debug), consoleOutput)) {
        std::cerr << "Failed to initialize logger" << std::endl;
        return 1;
    }

    /** 创建HTTP服务器实例，监听所有网络接口 */
    HttpServer server("0.0.0.0", port);

    /** 配置服务器线程池大小 */
    server.setNumThreads(numThreads);

    /** 应用可热更新配置 */
    applyRuntimeConfig(server, config, docRoot);

    /** 保存全局指针以便信号处理函数使用 */
    g_server = &server;

    /** 启动服务器并检查启动结果 */
    if (!server.start()) {
        LOG_ERROR("Failed to start server");
        return 1;
    }

    /** 服务器启动成功，输出提示信息 */
    LOG_INFO("Server is running. Press Ctrl+C to stop.");

    /**
     * @brief 主循环 - 保持服务器运行
     * 
     * 使用原子变量检查服务器运行状态，
     * 每秒检查一次以便响应关闭请求。
     * 
     * 注意：实际处理请求的工作在线程池中异步进行，
     * 这个主循环主要用于保持主线程存活。
     */
    while (server.isRunning()) {
        if (g_reloadRequested) {
            g_reloadRequested = 0;

            Config reloadedConfig;
            if (parseConfigFile(resolvedConfigFile, reloadedConfig)) {
                docRoot = resolvePathByConfigDir(reloadedConfig.docRoot, resolvedConfigFile);
                applyRuntimeConfig(server, reloadedConfig, docRoot);
                config = reloadedConfig;
                LOG_INFO("Reloaded config from: " + resolvedConfigFile);

                if (reloadedConfig.port != server.getPort()) {
                    LOG_WARN("Reloaded config changed port; restart is required to apply it");
                }
                if (reloadedConfig.threadPoolSize != server.getNumThreads()) {
                    LOG_WARN("Reloaded config changed thread pool size; restart is required to apply it");
                }
            } else {
                LOG_WARN("Failed to reload config from: " + resolvedConfigFile);
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
