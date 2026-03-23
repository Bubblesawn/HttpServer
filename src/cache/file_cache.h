/**
 * @file file_cache.h
 * @brief 静态文件缓存类的头文件定义
 * 
 * 本文件定义了FileCache类，提供基于LRU算法的静态文件内存缓存功能。
 * 缓存可以显著减少磁盘IO操作，提升静态文件服务的性能。
 */

#ifndef FILE_CACHE_H
#define FILE_CACHE_H

// C++标准库头文件
#include <string>              // 字符串处理
#include <memory>              // 智能指针
#include <unordered_map>       // 哈希映射
#include <list>                // 双向链表（用于LRU）
#include <mutex>               // 互斥锁
#include <chrono>              // 时间处理
#include <sys/stat.h>          // 文件状态

/**
 * @brief 文件缓存条目结构体
 * 
 * 存储缓存文件的元数据和内容。
 */
struct CacheEntry {
    std::string content;       // 文件内容
    std::string path;           // 文件路径
    time_t last_modified;      // 文件最后修改时间
    size_t size;                // 文件大小（字节）
    std::chrono::steady_clock::time_point access_time;  // 最后访问时间
    std::list<std::string>::iterator lru_iter;          // LRU链表迭代器
};

/**
 * @brief 文件缓存类
 * 
 * 使用LRU（最近最少使用）算法管理静态文件缓存。
 * 主要功能：
 * - 自动缓存小文件（可配置大小限制）
 * - 文件修改检测，自动失效过期缓存
 * - 缓存大小限制，超出时淘汰最久未使用的文件
 * - 线程安全，支持并发访问
 * 
 * 使用示例：
 * @code
 * FileCache cache;
 * cache.setMaxSize(100 * 1024 * 1024);  // 100MB
 * cache.setMaxFileSize(1024 * 1024);     // 单文件最大1MB
 * 
 * // 获取缓存文件
 * auto content = cache.get("/path/to/file.html");
 * if (content) {
 *     // 从缓存获取成功
 * } else {
 *     // 缓存未命中，需要从磁盘读取
 *     std::string fileContent = readFileFromDisk("/path/to/file.html");
 *     cache.put("/path/to/file.html", fileContent);
 * }
 * @endcode
 */
class FileCache {
public:
    /**
     * @brief 构造函数
     * 
     * 初始化文件缓存，设置默认配置。
     */
    FileCache();

    /**
     * @brief 析构函数
     * 
     * 清理缓存资源。
     */
    ~FileCache();

    /**
     * @brief 获取缓存文件内容
     * 
     * 根据文件路径从缓存中获取文件内容。
     * 如果缓存命中，会更新该文件的访问时间（LRU）。
     * 如果文件已被修改（通过比较文件修改时间），缓存会失效。
     * 
     * @param path 文件路径
     * @return std::shared_ptr<std::string> 缓存内容的共享指针，未命中返回nullptr
     */
    std::shared_ptr<std::string> get(const std::string& path);

    /**
     * @brief 添加文件到缓存
     * 
     * 将文件内容添加到缓存中。
     * 如果缓存大小超出限制，会自动淘汰最久未使用的文件。
     * 
     * @param path 文件路径
     * @param content 文件内容
     * @param last_modified 文件最后修改时间（用于缓存失效检测）
     */
    void put(const std::string& path, const std::string& content, time_t last_modified);

    /**
     * @brief 使指定文件的缓存失效
     * 
     * 从缓存中移除指定文件，通常在文件被修改或删除时调用。
     * 
     * @param path 文件路径
     */
    void invalidate(const std::string& path);

    /**
     * @brief 清空所有缓存
     * 
     * 移除所有缓存条目，释放内存。
     */
    void clear();

    /**
     * @brief 设置最大缓存大小
     * 
     * @param maxSize 最大缓存大小（字节）
     */
    void setMaxSize(size_t maxSize);

    /**
     * @brief 获取最大缓存大小
     * 
     * @return size_t 最大缓存大小（字节）
     */
    size_t getMaxSize() const;

    /**
     * @brief 设置单文件最大缓存大小
     * 
     * 超过此大小的文件不会被缓存。
     * 
     * @param maxSize 单文件最大缓存大小（字节）
     */
    void setMaxFileSize(size_t maxSize);

    /**
     * @brief 获取单文件最大缓存大小
     * 
     * @return size_t 单文件最大缓存大小（字节）
     */
    size_t getMaxFileSize() const;

    /**
     * @brief 检查文件是否应该被缓存
     * 
     * 根据文件类型和大小判断是否应该缓存该文件。
     * 
     * @param path 文件路径
     * @param size 文件大小
     * @return bool 应该缓存返回true，否则返回false
     */
    bool shouldCache(const std::string& path, size_t size) const;

    /**
     * @brief 获取当前缓存大小
     * 
     * @return size_t 当前缓存大小（字节）
     */
    size_t getCurrentSize() const;

    /**
     * @brief 获取缓存文件数量
     * 
     * @return size_t 缓存文件数量
     */
    size_t getCacheCount() const;

    /**
     * @brief 获取缓存命中次数
     * 
     * @return size_t 缓存命中次数
     */
    size_t getHitCount() const;

    /**
     * @brief 获取缓存未命中次数
     * 
     * @return size_t 缓存未命中次数
     */
    size_t getMissCount() const;

    /**
     * @brief 计算缓存命中率
     * 
     * @return double 缓存命中率（0.0 - 1.0）
     */
    double getHitRate() const;

    /**
     * @brief 重置统计信息
     * 
     * 清空命中和未命中计数器。
     */
    void resetStats();

    /**
     * @brief 启用或禁用缓存
     * 
     * @param enabled true启用缓存，false禁用缓存
     */
    void setEnabled(bool enabled);

    /**
     * @brief 检查缓存是否启用
     * 
     * @return bool 缓存启用返回true，否则返回false
     */
    bool isEnabled() const;

private:
    /**
     * @brief 检查文件是否为静态文件
     * 
     * 根据文件扩展名判断是否为静态文件。
     * 
     * @param path 文件路径
     * @return bool 是静态文件返回true，否则返回false
     */
    bool isStaticFile(const std::string& path) const;

    /**
     * @brief 获取文件扩展名
     * 
     * @param path 文件路径
     * @return std::string 文件扩展名（小写，不包含点号）
     */
    std::string getFileExtension(const std::string& path) const;

    /**
     * @brief 获取文件最后修改时间
     * 
     * @param path 文件路径
     * @return time_t 文件最后修改时间，失败返回-1
     */
    time_t getFileLastModified(const std::string& path) const;

    /**
     * @brief 淘汰最久未使用的缓存条目
     * 
     * 当缓存大小超出限制时，从LRU链表尾部开始淘汰条目。
     * 
     * @param sizeNeeded 需要释放的空间大小
     */
    void evict(size_t sizeNeeded);

    /**
     * @brief 更新LRU访问时间
     * 
     * 将访问的文件移动到LRU链表头部。
     * 
     * @param path 文件路径
     */
    void updateLRU(const std::string& path);

    // 成员变量
    std::unordered_map<std::string, CacheEntry> cache_;  // 缓存哈希表
    std::list<std::string> lru_list_;                     // LRU链表（头部为最近使用，尾部为最久未使用）
    size_t max_size_;                                     // 最大缓存大小（默认100MB）
    size_t current_size_;                                  // 当前缓存大小
    size_t max_file_size_;                                // 单文件最大缓存大小（默认1MB）
    std::mutex mutex_;                                    // 互斥锁，保证线程安全
    size_t hit_count_;                                    // 缓存命中次数
    size_t miss_count_;                                   // 缓存未命中次数
    bool enabled_;                                        // 缓存是否启用
};

#endif // FILE_CACHE_H
