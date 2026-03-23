/**
 * @file file_cache.cc
 * @brief 静态文件缓存类的实现文件
 * 
 * 实现FileCache类中声明的所有方法，
 * 包括LRU缓存管理、线程安全、缓存策略等功能。
 */

#include "../cache/file_cache.h"

// C++标准库头文件
#include <algorithm>    // 算法
#include <fstream>      // 文件流
#include <unordered_set> // 无序集合

/**
 * @brief 构造函数
 * 
 * 初始化文件缓存，设置默认配置参数。
 */
FileCache::FileCache()
    : max_size_(100 * 1024 * 1024)    // 默认最大缓存100MB
    , current_size_(0)
    , max_file_size_(1 * 1024 * 1024)  // 默认单文件最大1MB
    , hit_count_(0)
    , miss_count_(0)
    , enabled_(true) {
}

/**
 * @brief 析构函数
 * 
 * 清理缓存资源。
 */
FileCache::~FileCache() {
    clear();  // 清空所有缓存
}

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
std::shared_ptr<std::string> FileCache::get(const std::string& path) {
    // 如果缓存未启用，直接返回nullptr
    if (!enabled_) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cache_.find(path);
    if (it == cache_.end()) {
        // 缓存未命中
        miss_count_++;
        return nullptr;
    }

    CacheEntry& entry = it->second;

    // 检查文件是否已被修改
    time_t currentModified = getFileLastModified(path);
    if (currentModified != entry.last_modified) {
        // 文件已被修改，失效缓存
        cache_.erase(it);
        lru_list_.erase(entry.lru_iter);
        current_size_ -= entry.size;
        miss_count_++;
        return nullptr;
    }

    // 更新LRU访问时间（移动到链表头部）
    lru_list_.erase(entry.lru_iter);
    lru_list_.push_front(path);
    entry.lru_iter = lru_list_.begin();
    entry.access_time = std::chrono::steady_clock::now();

    // 缓存命中
    hit_count_++;

    return std::make_shared<std::string>(entry.content);
}

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
void FileCache::put(const std::string& path, const std::string& content, time_t last_modified) {
    // 如果缓存未启用，不执行任何操作
    if (!enabled_) {
        return;
    }

    // 检查文件是否应该被缓存
    if (!shouldCache(path, content.size())) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    size_t contentSize = content.size();

    // 如果文件已经存在于缓存中，先移除旧条目
    auto it = cache_.find(path);
    if (it != cache_.end()) {
        current_size_ -= it->second.size;
        lru_list_.erase(it->second.lru_iter);
        cache_.erase(it);
    }

    // 如果单个文件超过最大限制，不缓存
    if (contentSize > max_file_size_) {
        return;
    }

    // 如果需要淘汰足够的空间
    while (current_size_ + contentSize > max_size_ && !lru_list_.empty()) {
        evict(contentSize);
    }

    // 如果淘汰后仍然超出限制，不缓存
    if (current_size_ + contentSize > max_size_) {
        return;
    }

    // 创建新的缓存条目
    CacheEntry entry;
    entry.content = content;
    entry.path = path;
    entry.last_modified = last_modified;
    entry.size = contentSize;
    entry.access_time = std::chrono::steady_clock::now();

    // 添加到LRU链表头部
    lru_list_.push_front(path);
    entry.lru_iter = lru_list_.begin();

    // 添加到缓存哈希表
    cache_[path] = entry;
    current_size_ += contentSize;
}

/**
 * @brief 使指定文件的缓存失效
 * 
 * 从缓存中移除指定文件，通常在文件被修改或删除时调用。
 * 
 * @param path 文件路径
 */
void FileCache::invalidate(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cache_.find(path);
    if (it != cache_.end()) {
        current_size_ -= it->second.size;
        lru_list_.erase(it->second.lru_iter);
        cache_.erase(it);
    }
}

/**
 * @brief 清空所有缓存
 * 
 * 移除所有缓存条目，释放内存。
 */
void FileCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
    lru_list_.clear();
    current_size_ = 0;
}

/**
 * @brief 设置最大缓存大小
 * 
 * @param maxSize 最大缓存大小（字节）
 */
void FileCache::setMaxSize(size_t maxSize) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_size_ = maxSize;

    // 如果当前大小超出新限制，淘汰多余的缓存
    while (current_size_ > max_size_ && !lru_list_.empty()) {
        evict(current_size_ - max_size_);
    }
}

/**
 * @brief 获取最大缓存大小
 * 
 * @return size_t 最大缓存大小（字节）
 */
size_t FileCache::getMaxSize() const {
    return max_size_;
}

/**
 * @brief 设置单文件最大缓存大小
 * 
 * 超过此大小的文件不会被缓存。
 * 
 * @param maxSize 单文件最大缓存大小（字节）
 */
void FileCache::setMaxFileSize(size_t maxSize) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_file_size_ = maxSize;
}

/**
 * @brief 获取单文件最大缓存大小
 * 
 * @return size_t 单文件最大缓存大小（字节）
 */
size_t FileCache::getMaxFileSize() const {
    return max_file_size_;
}

/**
 * @brief 检查文件是否应该被缓存
 * 
 * 根据文件类型和大小判断是否应该缓存该文件。
 * 
 * @param path 文件路径
 * @param size 文件大小
 * @return bool 应该缓存返回true，否则返回false
 */
bool FileCache::shouldCache(const std::string& path, size_t size) const {
    // 不缓存空文件
    if (size == 0) {
        return false;
    }

    // 不缓存大文件
    if (size > max_file_size_) {
        return false;
    }

    // 只缓存静态文件
    return isStaticFile(path);
}

/**
 * @brief 获取当前缓存大小
 * 
 * @return size_t 当前缓存大小（字节）
 */
size_t FileCache::getCurrentSize() const {
    return current_size_;
}

/**
 * @brief 获取缓存文件数量
 * 
 * @return size_t 缓存文件数量
 */
size_t FileCache::getCacheCount() const {
    return cache_.size();
}

/**
 * @brief 获取缓存命中次数
 * 
 * @return size_t 缓存命中次数
 */
size_t FileCache::getHitCount() const {
    return hit_count_;
}

/**
 * @brief 获取缓存未命中次数
 * 
 * @return size_t 缓存未命中次数
 */
size_t FileCache::getMissCount() const {
    return miss_count_;
}

/**
 * @brief 计算缓存命中率
 * 
 * @return double 缓存命中率（0.0 - 1.0）
 */
double FileCache::getHitRate() const {
    size_t total = hit_count_ + miss_count_;
    if (total == 0) {
        return 0.0;
    }
    return static_cast<double>(hit_count_) / static_cast<double>(total);
}

/**
 * @brief 重置统计信息
 * 
 * 清空命中和未命中计数器。
 */
void FileCache::resetStats() {
    std::lock_guard<std::mutex> lock(mutex_);
    hit_count_ = 0;
    miss_count_ = 0;
}

/**
 * @brief 启用或禁用缓存
 * 
 * @param enabled true启用缓存，false禁用缓存
 */
void FileCache::setEnabled(bool enabled) {
    enabled_ = enabled;
}

/**
 * @brief 检查缓存是否启用
 * 
 * @return bool 缓存启用返回true，否则返回false
 */
bool FileCache::isEnabled() const {
    return enabled_;
}

/**
 * @brief 检查文件是否为静态文件
 * 
 * 根据文件扩展名判断是否为静态文件。
 * 
 * @param path 文件路径
 * @return bool 是静态文件返回true，否则返回false
 */
bool FileCache::isStaticFile(const std::string& path) const {
    // 允许缓存的文件扩展名
    static const std::unordered_set<std::string> staticExtensions = {
        "html", "htm", "css", "js", "json", "xml",
        "txt", "md", "markdown",
        "png", "jpg", "jpeg", "gif", "bmp", "ico", "svg", "webp",
        "woff", "woff2", "ttf", "eot", "otf",
        "pdf", "zip", "gz", "tar", "rar",
        "mp3", "mp4", "webm", "ogg", "wav",
        "woff", "woff2"
    };

    std::string ext = getFileExtension(path);
    return staticExtensions.find(ext) != staticExtensions.end();
}

/**
 * @brief 获取文件扩展名
 * 
 * @param path 文件路径
 * @return std::string 文件扩展名（小写，不包含点号）
 */
std::string FileCache::getFileExtension(const std::string& path) const {
    size_t dotPos = path.find_last_of('.');
    size_t slashPos = path.find_last_of('/');
    
    // 如果点号在最后一个斜杠之后，才是有效的扩展名
    if (dotPos != std::string::npos && (slashPos == std::string::npos || dotPos > slashPos)) {
        std::string ext = path.substr(dotPos + 1);
        // 转换为小写
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return ext;
    }
    
    return "";
}

/**
 * @brief 获取文件最后修改时间
 * 
 * @param path 文件路径
 * @return time_t 文件最后修改时间，失败返回-1
 */
time_t FileCache::getFileLastModified(const std::string& path) const {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return st.st_mtime;
    }
    return -1;
}

/**
 * @brief 淘汰最久未使用的缓存条目
 * 
 * 当缓存大小超出限制时，从LRU链表尾部开始淘汰条目。
 * 
 * @param sizeNeeded 需要释放的空间大小
 */
void FileCache::evict(size_t sizeNeeded) {
    while (sizeNeeded > 0 && !lru_list_.empty()) {
        // 从LRU链表尾部获取最久未使用的文件路径
        std::string lruPath = lru_list_.back();
        lru_list_.pop_back();

        auto it = cache_.find(lruPath);
        if (it != cache_.end()) {
            size_t removedSize = it->second.size;
            cache_.erase(it);
            current_size_ -= removedSize;
            sizeNeeded -= removedSize;
        }
    }
}

/**
 * @brief 更新LRU访问时间
 * 
 * 将访问的文件移动到LRU链表头部。
 * 
 * @param path 文件路径
 */
void FileCache::updateLRU(const std::string& path) {
    auto it = cache_.find(path);
    if (it != cache_.end()) {
        lru_list_.erase(it->second.lru_iter);
        lru_list_.push_front(path);
        it->second.lru_iter = lru_list_.begin();
        it->second.access_time = std::chrono::steady_clock::now();
    }
}
