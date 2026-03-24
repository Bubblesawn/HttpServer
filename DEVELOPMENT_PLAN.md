# HttpServer 详细开发方案

## 项目概述

这是一个基于 **C++17 / Linux** 实现的高性能 HTTP/1.1 静态文件服务器，采用 **epoll ET 边缘触发 + 线程池** 的经典并发架构。

### 当前技术架构

```
┌─────────────────────────────────────────────────┐
│                   main.cc                        │
│  配置解析 / 信号注册 / 日志初始化 / 服务器启动      │
└────────────────────┬────────────────────────────┘
                     │
         ┌───────────▼───────────┐
         │      HttpServer       │
         │  TCP Accept + Epoll   │
         │  事件分发 + 静态文件    │
         └─────┬─────────┬───────┘
               │         │
       ┌───────▼──┐  ┌───▼────────┐
       │ThreadPool│  │EpollManager│
       │ 20 线程  │  │ ET模式监听 │
       └──────────┘  └────────────┘
               │
   ┌───────────┼───────────┐
   │           │           │
┌──▼──┐  ┌────▼──┐  ┌─────▼───┐
│Http │  │Http   │  │FileCache│
│Req  │  │Resp   │  │LRU缓存  │
└─────┘  └───────┘  └─────────┘
                         │
                    ┌────▼────┐
                    │ Logger  │
                    │单例/分级│
                    └─────────┘
```

### 已实现功能清单

| 功能 | 状态 | 备注 |
|------|------|------|
| TCP Server + Accept 循环 | ✅ | 支持 epoll / 传统双模式 |
| epoll ET 边缘触发 | ✅ | `EpollManager` |
| 固定大小线程池 | ✅ | 条件变量任务队列 |
| HTTP 请求解析 | ✅ | GET/POST/PUT/DELETE/HEAD |
| 静态文件服务 | ✅ | 含 MIME 类型推断 |
| 目录列表自动生成 | ✅ | 无 index.html 时 |
| 路径遍历攻击防护 | ✅ | `isPathTraversal()` |
| LRU 文件缓存 | ✅ | 可配置大小 / 命中率统计 |
| 分级日志系统 | ✅ | DEBUG/INFO/WARN/ERROR 双文件 |
| 配置文件解析 | ✅ | `http_server.conf` |
| 优雅关闭 | ✅ | SIGINT / SIGTERM |
| /api/echo 测试端点 | ✅ | 返回请求信息 JSON |

---

## 待开发事项分析

### 现有短板

1. **无持久连接**：每次请求后立即关闭连接，高并发下 TCP 握手开销大
2. **无大文件优化**：大文件通过内存拷贝发送，无 `sendfile()` 零拷贝
3. **无压缩**：传输 HTML/JS/CSS 未压缩，浪费带宽
4. **无路由系统**：所有自定义逻辑都在一个 handler 里，扩展性差
5. **无连接超时**：不活跃连接长期占用 fd 资源
6. **无速率限制**：易被 DoS 攻击
7. **无单元测试**：无测试框架接入
8. **无状态监控**：无法实时了解服务器运行指标

---

## 开发计划（七个阶段）

---

### 阶段 0：环境与构建（前置）

> 目标：摸清构建环境，建立统一的编译基线。

#### 任务
- 检查是否存在 `Makefile` / `CMakeLists.txt`，若缺失需新建
- 建议统一使用 **CMake 3.16+** 管理构建
- 建立 `build/` 输出目录，保持源码目录整洁
- 加入 `-Wall -Wextra -fsanitize=address` 调试编译选项

#### 新增文件
```
CMakeLists.txt          ← 项目根（若不存在则新建）
cmake/CompilerOptions.cmake
```

---

### 阶段 1：核心功能增强

> 目标：补齐 HTTP/1.1 协议关键特性，提升兼容性与性能。

#### 1.1 HTTP Keep-Alive 持久连接

**原理**：复用 TCP 连接处理多个请求，避免频繁握手。

| 文件 | 变更说明 |
|------|----------|
| [MODIFY] [http_server.h](file:///e:/HttpServer/src/server/http_server.h) | 增加 keep-alive 超时配置项 `m_keepAliveTimeout` |
| [MODIFY] [http_server.cc](file:///e:/HttpServer/src/server/http_server.cc) | `handleClient()` 改为循环处理同一连接的多次请求；读取 `Connection: keep-alive` 头部决定是否保持连接 |

**关键逻辑**：
```
Connection: keep-alive → 响应后不关闭 socket，epoll 继续监听
Connection: close     → 响应后关闭 socket，释放资源
超时（idle > N秒）    → 主动关闭连接
```

#### 1.2 大文件 sendfile() 零拷贝

**原理**：`sendfile(out_fd, in_fd, offset, count)` 在内核态完成文件到 socket 的数据传输，避免用户态拷贝。

| 文件 | 变更说明 |
|------|----------|
| [MODIFY] [http_response.h](file:///e:/HttpServer/src/response/http_response.h) | 增加 `m_useSendFile` 标志 |
| [MODIFY] [http_server.cc](file:///e:/HttpServer/src/server/http_server.cc) | 大文件（>缓存阈值）走 `sendfile()`，小文件走 `FileCache` |

#### 1.3 Range 请求支持（断点续传）

| 文件 | 变更说明 |
|------|----------|
| [MODIFY] [http_server.cc](file:///e:/HttpServer/src/server/http_server.cc) | 解析 `Range: bytes=start-end` 头部，返回 `206 Partial Content` + `Content-Range` 头 |

#### 1.4 Gzip 压缩

| 文件 | 变更说明 |
|------|----------|
| [NEW] [compressor.h](file:///e:/HttpServer/src/utils/compressor.h) | 基于 zlib 封装 gzip 压缩工具类 |
| [NEW] [compressor.cc](file:///e:/HttpServer/src/utils/compressor.cc) | 实现 `compress(string) → string` |
| [MODIFY] [http_server.cc](file:///e:/HttpServer/src/server/http_server.cc) | 检测 `Accept-Encoding: gzip`，对文本类响应压缩后返回 |

#### 1.5 Router 路由系统

| 文件 | 变更说明 |
|------|----------|
| [NEW] [router.h](file:///e:/HttpServer/src/router/router.h) | Route 注册表：`method + path → handler`，支持路径参数 `/user/:id` |
| [NEW] [router.cc](file:///e:/HttpServer/src/router/router.cc) | Trie 树实现路由匹配 |
| [MODIFY] [http_server.h](file:///e:/HttpServer/src/server/http_server.h) | 增加 `addRoute(method, path, handler)` 公开接口 |

---

### 阶段 2：安全加固

> 目标：防范常见 HTTP 攻击，提升生产环境可靠性。

#### 2.1 请求体大小限制

| 文件 | 变更说明 |
|------|----------|
| [MODIFY] [http_server.h](file:///e:/HttpServer/src/server/http_server.h) | 增加 `m_maxBodySize`（默认 10MB）配置项 |
| [MODIFY] [http_server.cc](file:///e:/HttpServer/src/server/http_server.cc) | 读取 body 时若超过阈值返回 `413 Payload Too Large` |
| [MODIFY] [http_server.conf](file:///e:/HttpServer/http_server.conf) | 新增 `max_body_size 10485760` 配置项 |

#### 2.2 连接速率限制

| 文件 | 变更说明 |
|------|----------|
| [NEW] [rate_limiter.h](file:///e:/HttpServer/src/security/rate_limiter.h) | 令牌桶算法实现：`isAllowed(ip) → bool` |
| [NEW] [rate_limiter.cc](file:///e:/HttpServer/src/security/rate_limiter.cc) | 按 IP 维护令牌桶，超速返回 `429 Too Many Requests` |
| [MODIFY] [http_server.cc](file:///e:/HttpServer/src/server/http_server.cc) | accept 新连接时先通过速率限制检查 |

#### 2.3 请求超时管理

| 文件 | 变更说明 |
|------|----------|
| [MODIFY] [http_server.h](file:///e:/HttpServer/src/server/http_server.h) | 增加 `m_readTimeout`（读超时，秒）、`m_idleTimeout`（空闲超时，秒）|
| [MODIFY] [http_server.cc](file:///e:/HttpServer/src/server/http_server.cc) | 维护连接上次活跃时间戳，定时扫描超时连接并关闭 |

#### 2.4 安全响应头

| 文件 | 变更说明 |
|------|----------|
| [MODIFY] [http_response.cc](file:///e:/HttpServer/src/response/http_response.cc) | 默认添加 `X-Content-Type-Options: nosniff`、`X-Frame-Options: SAMEORIGIN`、`Referrer-Policy: strict-origin` |

---

### 阶段 3：性能优化

> 目标：在现有 epoll 架构基础上进一步压榨性能。

#### 3.1 线程池动态扩缩容

| 文件 | 变更说明 |
|------|----------|
| [MODIFY] [thread_pool.h](file:///e:/HttpServer/src/thread/thread_pool.h) | 增加 `min_threads`、`max_threads`、`idle_timeout` 配置 |
| [MODIFY] [thread_pool.cc](file:///e:/HttpServer/src/thread/thread_pool.cc) | 后台监控线程：队列积压时扩容，线程空闲超时时缩容 |

#### 3.2 配置热重载

| 文件 | 变更说明 |
|------|----------|
| [MODIFY] [main.cc](file:///e:/HttpServer/src/main/main.cc) | 注册 `SIGHUP` 信号处理，重新解析配置文件并应用 |

#### 3.3 缓存预热

| 文件 | 变更说明 |
|------|----------|
| [MODIFY] [file_cache.cc](file:///e:/HttpServer/src/cache/file_cache.cc) | 启动时扫描 `doc_root` 下的静态文件并预先加载到缓存 |

---

### 阶段 4：可观测性 & 运维

> 目标：提供实时监控与管理能力。

#### 4.1 /status 管理端点

| 文件 | 变更说明 |
|------|----------|
| [NEW] [stats_collector.h](file:///e:/HttpServer/src/monitor/stats_collector.h) | 全局原子计数器：总请求数、错误数、活跃连接数、平均响应时间 |
| [MODIFY] [http_server.cc](file:///e:/HttpServer/src/server/http_server.cc) | `GET /status` → 返回 JSON 统计信息 |

**响应示例**：
```json
{
  "uptime_seconds": 3600,
  "total_requests": 100000,
  "active_connections": 42,
  "cache_hit_rate": 0.93,
  "threads": 20
}
```

#### 4.2 /health 健康检查

| 文件 | 变更说明 |
|------|----------|
| [MODIFY] [http_server.cc](file:///e:/HttpServer/src/server/http_server.cc) | `GET /health` → `200 {"status":"ok"}` |

#### 4.3 日志轮转

| 文件 | 变更说明 |
|------|----------|
| [MODIFY] [logger.h](file:///e:/HttpServer/src/logger/logger.h) | 增加 `m_maxLogSize`、`m_maxBackups` 配置 |
| [MODIFY] [logger.cc](file:///e:/HttpServer/src/logger/logger.cc) | 写入前检查文件大小，超限则 rename 备份后新建 |

#### 4.4 容器化部署

| 文件 | 变更说明 |
|------|----------|
| [NEW] [Dockerfile](file:///e:/HttpServer/Dockerfile) | 多阶段构建：builder（g++ 编译）+ runner（alpine 运行） |
| [NEW] [docker-compose.yml](file:///e:/HttpServer/docker-compose.yml) | 端口映射、卷挂载、环境变量配置 |

---

### 阶段 5：测试体系

> 目标：建立可信赖的自动化测试基线。

#### 5.1 单元测试（Google Test）

| 测试模块 | 测试文件 | 主要用例 |
|----------|----------|----------|
| HttpRequest | `tests/test_http_request.cc` | 方法解析、头部解析、URL 解析、查询参数 |
| HttpResponse | `tests/test_http_response.cc` | 状态码、序列化 `toString()`、工厂方法 |
| FileCache | `tests/test_file_cache.cc` | LRU 淘汰、命中/未命中、线程安全 |
| RateLimiter | `tests/test_rate_limiter.cc` | 令牌桶逻辑、超速检测 |
| Router | `tests/test_router.cc` | 路由注册、参数提取、404 |

**运行方式**：
```bash
mkdir build && cd build
cmake .. -DENABLE_TESTS=ON
make -j4
ctest --test-dir . -V
```

#### 5.2 集成测试（curl 脚本）

| 文件 | 说明 |
|------|------|
| [NEW] [tests/integration/test_basic.sh](file:///e:/HttpServer/tests/integration/test_basic.sh) | GET 静态文件、POST /api/echo、404 测试 |
| [NEW] [tests/integration/test_security.sh](file:///e:/HttpServer/tests/integration/test_security.sh) | 路径遍历、大 body、速率限制测试 |

**运行方式**：
```bash
./scripts/start.sh &
sleep 1
bash tests/integration/test_basic.sh
bash tests/integration/test_security.sh
```

#### 5.3 压力测试（wrk）

```bash
# 静态文件压测（10线程×100并发，持续30秒）
wrk -t10 -c100 -d30s http://localhost:8080/index.html

# POST 接口压测
wrk -t4 -c50 -d10s -s tests/post.lua http://localhost:8080/api/echo
```

---

### 阶段 6：文档完善

| 文档 | 路径 | 内容 |
|------|------|------|
| API 文档 | `docs/api.md` | 所有端点说明、请求/响应示例 |
| 配置参考 | `docs/configuration.md` | 所有配置项说明、默认值、范围 |
| 架构说明 | `docs/architecture.md` | 模块关系图、核心流程图 |
| CHANGELOG | `CHANGELOG.md` | 版本变更记录 |
| CONTRIBUTING | `CONTRIBUTING.md` | 提交规范、开发流程 |
| Doxygen | `Doxyfile` | 自动生成 HTML API 文档 |

---

## 开发优先级推荐

```
P0（立刻做）: 0-构建基线 → 5.1-单元测试框架 → 1.1-Keep-Alive
P1（近期）:   1.2-sendfile → 2.1-体积限制 → 2.3-超时 → 4.1-/status
P2（中期）:   1.4-Gzip → 1.5-Router → 2.2-速率限制 → 3.1-动态线程池
P3（长期）:   4.4-容器化 → 6-文档 → 3.2-热重载
```

---

## 验证计划

### 每个阶段的验证方式

| 阶段 | 验证命令 / 步骤 |
|------|----------------|
| Keep-Alive | `curl -v --keepalive http://localhost:8080/` 查看连接复用 |
| sendfile | `wrk -t4 -c50 -d10s http://localhost:8080/large_file.bin` 对比吞吐量 |
| Gzip | `curl -H "Accept-Encoding: gzip" -I http://localhost:8080/index.html` 检查 `Content-Encoding: gzip` |
| 速率限制 | 连续快速发送 1000 请求，检查是否出现 `429` |
| /status | `curl http://localhost:8080/status` 返回合法 JSON |
| 单元测试 | `ctest --test-dir build -V` 全部通过 |
| 压力测试 | wrk 结果：RPS > 50,000（本地环境预期值）|
