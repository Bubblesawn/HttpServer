/**
 * 抖音风格视频播放页面 - JavaScript交互逻辑
 * 作者: AI Assistant
 * 功能: 处理视频播放、滑动切换、点赞等交互功能
 */

// ==================== 全局变量定义 ====================

// 当前视频索引
let currentIndex = 0;

// 视频总数
let videoCount = 0;

// 触摸起始位置
let touchStartY = 0;

// 触摸结束位置
let touchEndY = 0;

// 是否正在切换视频
let isTransitioning = false;

// 视频播放状态数组
let videoStates = [];

// ==================== 页面加载完成后的初始化 ====================

// 等待DOM加载完成后执行
document.addEventListener('DOMContentLoaded', function() {
    // 初始化视频相关功能
    initVideos();
    
    // 初始化触摸滑动事件
    initTouchEvents();
    
    // 初始化点击播放/暂停事件
    initClickEvents();
    
    // 初始化顶部标签切换事件
    initTabEvents();
    
    // 初始化底部导航事件
    initNavEvents();
});

/**
 * 初始化视频功能
 * 设置视频数量、状态数组，并为第一个视频添加加载完成事件
 */
function initVideos() {
    // 获取所有视频元素
    const videoItems = document.querySelectorAll('.video-item');
    
    // 获取视频总数
    videoCount = videoItems.length;
    
    // 初始化每个视频的播放状态
    videoItems.forEach(function(item, index) {
        // 设置初始状态为未播放
        videoStates[index] = {
            isPlaying: false,
            isLiked: false
        };
        
        // 获取视频元素
        const video = item.querySelector('.video-player');
        
        // 监听视频加载元数据完成事件
        video.addEventListener('loadedmetadata', function() {
            // 隐藏加载图标
            const loading = item.querySelector('.video-loading');
            if (loading) {
                loading.style.display = 'none';
            }
        });
        
        // 监听视频播放开始事件
        video.addEventListener('play', function() {
            // 隐藏播放图标
            const playIcon = item.querySelector('.play-icon');
            if (playIcon) {
                playIcon.style.display = 'none';
            }
            // 移除暂停状态类
            item.classList.remove('paused');
        });
        
        // 监听视频暂停事件
        video.addEventListener('pause', function() {
            // 显示播放图标
            const playIcon = item.querySelector('.play-icon');
            if (playIcon) {
                playIcon.style.display = 'block';
            }
            // 添加暂停状态类
            item.classList.add('paused');
        });
        
        // 监听视频错误事件
        video.addEventListener('error', function() {
            // 隐藏加载图标
            const loading = item.querySelector('.video-loading');
            if (loading) {
                loading.style.display = 'none';
            }
            console.error('视频加载失败: ' + video.src);
        });
    });
    
    // 自动播放第一个视频
    playVideo(0);
}

/**
 * 初始化触摸滑动事件
 * 处理上下滑动切换视频的逻辑
 */
function initTouchEvents() {
    // 获取视频容器
    const container = document.getElementById('videoContainer');
    
    // 监听触摸开始事件
    container.addEventListener('touchstart', function(e) {
        // 获取触摸起始位置
        touchStartY = e.touches[0].clientY;
    }, { passive: true });
    
    // 监听触摸移动事件
    container.addEventListener('touchmove', function(e) {
        // 阻止默认滚动行为
        e.preventDefault();
    }, { passive: false });
    
    // 监听触摸结束事件
    container.addEventListener('touchend', function(e) {
        // 获取触摸结束位置
        touchEndY = e.changedTouches[0].clientY;
        
        // 计算滑动距离
        const diffY = touchStartY - touchEndY;
        
        // 设置最小滑动距离阈值为50px
        const minSwipeDistance = 50;
        
        // 判断是否达到滑动阈值
        if (Math.abs(diffY) > minSwipeDistance) {
            // 如果正在切换视频，直接返回
            if (isTransitioning) {
                return;
            }
            
            // 判断滑动方向
            if (diffY > 0) {
                // 向上滑动，播放下一个视频
                playNextVideo();
            } else {
                // 向下滑动，播放上一个视频
                playPrevVideo();
            }
        }
    });
    
    // 为桌面端添加鼠标拖动支持
    let isDragging = false;
    let dragStartY = 0;
    let dragEndY = 0;
    
    container.addEventListener('mousedown', function(e) {
        isDragging = true;
        dragStartY = e.clientY;
    });
    
    container.addEventListener('mousemove', function(e) {
        if (!isDragging) return;
        e.preventDefault();
    });
    
    container.addEventListener('mouseup', function(e) {
        if (!isDragging) return;
        isDragging = false;
        dragEndY = e.clientY;
        
        const diffY = dragStartY - dragEndY;
        const minSwipeDistance = 50;
        
        if (Math.abs(diffY) > minSwipeDistance) {
            if (isTransitioning) return;
            
            if (diffY > 0) {
                playNextVideo();
            } else {
                playPrevVideo();
            }
        }
    });
    
    container.addEventListener('mouseleave', function() {
        isDragging = false;
    });
}

/**
 * 初始化点击播放/暂停事件
 * 点击视频区域切换播放状态
 */
function initClickEvents() {
    // 获取视频容器
    const container = document.getElementById('videoContainer');
    
    // 监听点击事件
    container.addEventListener('click', function(e) {
        // 如果点击的是按钮或交互元素，不触发播放/暂停
        if (e.target.closest('.action-item') || 
            e.target.closest('.bottom-info') || 
            e.target.closest('.right-actions')) {
            return;
        }
        
        // 切换当前视频的播放状态
        togglePlayPause();
    });
}

/**
 * 初始化顶部标签切换事件
 */
function initTabEvents() {
    // 获取所有标签
    const tabs = document.querySelectorAll('.header-tabs .tab');
    
    // 为每个标签添加点击事件
    tabs.forEach(function(tab) {
        tab.addEventListener('click', function() {
            // 移除所有标签的激活状态
            tabs.forEach(function(t) {
                t.classList.remove('active');
            });
            
            // 为当前点击的标签添加激活状态
            this.classList.add('active');
        });
    });
}

/**
 * 初始化底部导航事件
 */
function initNavEvents() {
    // 获取所有导航项
    const navItems = document.querySelectorAll('.bottom-nav .nav-item');
    
    // 为每个导航项添加点击事件
    navItems.forEach(function(item) {
        item.addEventListener('click', function() {
            // 移除所有导航项的激活状态
            navItems.forEach(function(nav) {
                nav.classList.remove('active');
            });
            
            // 为当前点击的导航项添加激活状态
            this.classList.add('active');
        });
    });
}

// ==================== 视频播放控制函数 ====================

/**
 * 播放指定索引的视频
 * @param {number} index - 视频索引
 */
function playVideo(index) {
    // 边界检查
    if (index < 0 || index >= videoCount) {
        return;
    }
    
    // 获取所有视频项
    const videoItems = document.querySelectorAll('.video-item');
    
    // 遍历所有视频项
    videoItems.forEach(function(item, i) {
        // 获取视频元素
        const video = item.querySelector('.video-player');
        
        if (i === index) {
            // 设置当前视频项为激活状态
            item.classList.add('active');
            
            // 显示加载图标
            const loading = item.querySelector('.video-loading');
            if (loading) {
                loading.style.display = 'block';
            }
            
            // 播放视频
            video.play().then(function() {
                // 标记为播放状态
                videoStates[index].isPlaying = true;
            }).catch(function(error) {
                console.error('自动播放失败:', error);
                // 显示播放图标
                const playIcon = item.querySelector('.play-icon');
                if (playIcon) {
                    playIcon.style.display = 'block';
                }
                // 隐藏加载图标
                const loading = item.querySelector('.video-loading');
                if (loading) {
                    loading.style.display = 'none';
                }
            });
        } else {
            // 非当前视频项移除激活状态
            item.classList.remove('active');
            
            // 暂停视频
            video.pause();
            // 重置视频播放时间到开头
            video.currentTime = 0;
            
            // 标记为未播放状态
            videoStates[i].isPlaying = false;
        }
    });
    
    // 更新当前索引
    currentIndex = index;
}

/**
 * 播放下一个视频
 */
function playNextVideo() {
    // 设置正在切换状态
    isTransitioning = true;
    
    // 计算下一个视频索引（循环播放）
    const nextIndex = (currentIndex + 1) % videoCount;
    
    // 播放下一个视频
    playVideo(nextIndex);
    
    // 300ms后重置切换状态
    setTimeout(function() {
        isTransitioning = false;
    }, 300);
}

/**
 * 播放上一个视频
 */
function playPrevVideo() {
    // 设置正在切换状态
    isTransitioning = true;
    
    // 计算上一个视频索引（循环播放）
    const prevIndex = (currentIndex - 1 + videoCount) % videoCount;
    
    // 播放上一个视频
    playVideo(prevIndex);
    
    // 300ms后重置切换状态
    setTimeout(function() {
        isTransitioning = false;
    }, 300);
}

/**
 * 切换当前视频的播放/暂停状态
 */
function togglePlayPause() {
    // 获取当前视频项
    const videoItems = document.querySelectorAll('.video-item');
    const currentVideoItem = videoItems[currentIndex];
    
    // 获取当前视频元素
    const video = currentVideoItem.querySelector('.video-player');
    
    // 判断视频当前状态
    if (video.paused) {
        // 如果暂停，则播放
        video.play();
        videoStates[currentIndex].isPlaying = true;
    } else {
        // 如果正在播放，则暂停
        video.pause();
        videoStates[currentIndex].isPlaying = false;
    }
}

// ==================== 交互功能函数 ====================

/**
 * 切换点赞状态
 * @param {number} index - 视频索引
 */
function toggleLike(index) {
    // 获取对应视频的点赞按钮
    const videoItems = document.querySelectorAll('.video-item');
    const likeButton = videoItems[index].querySelector('.action-item.liked, .action-item:not(.avatar)');
    
    // 查找正确的点赞按钮（排除头像）
    const allActions = videoItems[index].querySelectorAll('.action-item');
    let likeItem = null;
    
    allActions.forEach(function(action) {
        if (!action.classList.contains('avatar') && action.querySelector('.fa-heart')) {
            likeItem = action;
        }
    });
    
    if (!likeItem) return;
    
    // 切换点赞状态
    const isLiked = likeItem.classList.toggle('liked');
    
    // 更新状态数组
    videoStates[index].isLiked = isLiked;
    
    // 获取点赞数量元素
    const countElement = likeItem.querySelector('.count');
    
    if (countElement) {
        // 解析当前数量
        let countText = countElement.textContent;
        let count = parseCount(countText);
        
        // 根据点赞状态增减数量
        if (isLiked) {
            count++;
        } else {
            count--;
        }
        
        // 更新数量显示
        countElement.textContent = formatCount(count);
    }
}

/**
 * 解析数量字符串为数字
 * @param {string} text - 数量文本（如 "12.8w"）
 * @returns {number} - 解析后的数字
 */
function parseCount(text) {
    // 移除空格
    text = text.trim().toLowerCase();
    
    // 判断单位
    if (text.includes('w')) {
        // 万单位
        return parseFloat(text) * 10000;
    } else if (text.includes('k')) {
        // 千单位
        return parseFloat(text) * 1000;
    } else {
        // 普通数字
        return parseInt(text) || 0;
    }
}

/**
 * 格式化数量为字符串
 * @param {number} count - 数量
 * @returns {string} - 格式化后的字符串
 */
function formatCount(count) {
    if (count >= 10000) {
        // 转换为万单位
        return (count / 10000).toFixed(1).replace(/\.0$/, '') + 'w';
    } else if (count >= 1000) {
        // 转换为千单位
        return (count / 1000).toFixed(1).replace(/\.0$/, '') + 'k';
    } else {
        // 直接返回
        return count.toString();
    }
}

// ==================== 工具函数 ====================

/**
 * 获取当前播放的视频索引
 * @returns {number} - 当前视频索引
 */
function getCurrentIndex() {
    return currentIndex;
}

/**
 * 获取指定视频的播放状态
 * @param {number} index - 视频索引
 * @returns {object} - 播放状态对象
 */
function getVideoState(index) {
    if (index >= 0 && index < videoStates.length) {
        return videoStates[index];
    }
    return null;
}

/**
 * 检查是否正在切换视频
 * @returns {boolean} - 是否正在切换
 */
function isSwiping() {
    return isTransitioning;
}
