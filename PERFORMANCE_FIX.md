# QPS 性能诊断与修复计划

> 问题现象：压测 QPS 仅 400，远低于 epoll + 线程池架构的预期性能（应 > 10,000）。

---

## 根因汇总

| # | 级别 | 位置 | 问题描述 | 预期收益 |
|---|------|------|----------|----------|
| 1 | 🔴 严重 | `logger.cc` | 每条日志同步 `flush()` 刷盘 + 互斥锁串行 | QPS 提升 3~5 倍 |
| 2 | 🔴 严重 | `http_server.cc:937` | `parseRequest()` 多余的 `select()` 系统调用 | QPS 提升 ~10% |
| 3 | 🟠 中等 | `http_server.cc:890` | 无 Keep-Alive，每请求强制 TCP 握手/挥手 | QPS 提升 2~3 倍 |
| 4 | 🟠 中等 | `http_server.cc:632` | 每连接 3 次 `setsockopt` 系统调用 | 小幅提升 |
| 5 | 🟡 轻微 | `http_server.cc:644` | `m_clientInfoMap` 全局 mutex 锁竞争 | 高并发下明显 |

---

## 详细修复方案

---

### 修复 1：日志同步刷盘 🔴（最高优先级）

#### 问题代码

```cpp
// logger.cc — 每次写日志都 flush 两次，每次都是同步磁盘 IO！
m_accessLogFile << logLine << std::endl;  // std::endl 本身触发 flush
m_accessLogFile.flush();                  // 再次显式 flush，双倍惩罚
```

所有线程写日志还共享同一把 `m_mutex`，导致日志写入完全串行化。

#### 修复方案 A：最简修复（改 `\n`，去掉 `flush`）

```cpp
// 把所有 std::endl 改为 "\n"，删除所有显式 flush()
m_accessLogFile << logLine << "\n";
// 删除：m_accessLogFile.flush();
```

**效果**：利用 `ofstream` 的 64KB 内核缓冲区，减少 99% 的磁盘写次数。

#### 修复方案 B：异步日志（推荐，彻底解决）

新增一个专用后台线程消费日志队列，主线程只做无锁入队：

```
写日志线程(enqueue)  →  无锁队列  →  后台 I/O 线程(dequeue + write)
```

**新增文件**：`src/logger/async_logger.h` / `async_logger.cc`

关键数据结构：
```cpp
std::queue<std::string> m_queue;    // 日志缓冲队列
std::mutex              m_mutex;
std::condition_variable m_cv;
std::thread             m_ioThread; // 专用 I/O 线程
```

**效果**：写日志变为纯内存操作，对主处理线程零阻塞。

---

### 修复 2：删除多余的 `select()` 🔴

#### 问题代码

```cpp
// http_server.cc L929~941 — parseRequest() 开头
// epoll 已通知数据就绪才到这里，再 select() 是白白的系统调用浪费
fd_set readFds;
FD_ZERO(&readFds);
FD_SET(clientSocket, &readFds);
struct timeval selectTimeout;
selectTimeout.tv_sec = 5;
int selectResult = select(clientSocket + 1, &readFds, nullptr, nullptr, &selectTimeout);
if (selectResult <= 0) return HttpRequest();
```

#### 修复方案

**直接删除**上述 9 行代码。epoll 水平触发（LT）在通知时数据必然就绪，无需再用 `select()` 二次确认。

```cpp
// 删除后，直接从读取请求行开始
if (readLine(clientSocket, line) <= 0) {
    return HttpRequest();
}
```

---

### 修复 3：实现 HTTP Keep-Alive 🟠

#### 问题代码

```cpp
// http_server.cc L889~890 — handleClient() 末尾
// 每次请求处理完立即关闭连接，完全没有连接复用
close(clientSocket);
```

#### 修复方案

在 `handleClient()` 中改为循环处理同一连接的多次请求：

```cpp
void HttpServer::handleClient(int clientSocket, ...) {
    while (true) {
        // 1. 解析请求
        HttpRequest request = parseRequest(clientSocket);
        if (invalid) break;

        // 2. 处理并发送响应
        HttpResponse response = dispatch(request);
        sendResponse(clientSocket, response);

        // 3. 判断是否保持连接
        std::string conn = request.getHeader("Connection");
        bool keepAlive = (conn == "keep-alive") ||
                         (version == "HTTP/1.1" && conn != "close");
        if (!keepAlive) break;

        // 4. HTTP/1.1 响应中需加上 Keep-Alive 头
        // Connection: keep-alive
        // Keep-Alive: timeout=5, max=100
    }
    close(clientSocket);
}
```

同时在 `handleClientRead()` 中，处理完后不要 `close()`，而是将 fd 重新加回 epoll 监听：

```cpp
// 处理完后重新注册到 epoll，等待下一次请求
m_epollManager->addFd(clientSocket, EpollEventType::READ, clientCallback, false);
// 不 close(clientSocket)
```

**涉及文件**：`src/server/http_server.h`（加 `m_keepAliveTimeout`）、`src/server/http_server.cc`

---

### 修复 4：减少 setsockopt 系统调用 🟠

#### 问题代码

```cpp
// http_server.cc L632~640 — 每 accept 一个连接都调用 3 次 setsockopt
setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
```

#### 修复方案

- 保留 `TCP_NODELAY`（减少延迟，有必要）。  
- 删除 `SO_RCVTIMEO` / `SO_SNDTIMEO`：用应用层超时（epoll 定时扫描）代替 socket 级别的超时设置，避免每连接两次系统调用。

---

### 修复 5：减少 m_clientInfoMap 锁竞争 🟡

#### 问题代码

```cpp
// 两个不同线程（accept 线程 + epoll 事件线程）都要抢这把锁
std::lock_guard<std::mutex> lock(m_clientInfoMutex);
m_clientInfoMap[fd] = {...};   // accept 线程写
m_clientInfoMap.find(fd);      // epoll 线程读
m_clientInfoMap.erase(fd);     // epoll 线程删
```

#### 修复方案

将 IP/Port 信息直接存入 `epoll_event.data.ptr` 指向的小结构体，完全绕过 `m_clientInfoMap`：

```cpp
// 为每个连接分配一个 ClientInfo 结构体
ClientInfo* info = new ClientInfo{ip, port};

struct epoll_event ev;
ev.data.ptr = info;  // 直接挂在 epoll 事件上
epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &ev);

// 事件触发时直接取出，无需任何锁
ClientInfo* info = static_cast<ClientInfo*>(event.data.ptr);
```

---

## 修复优先级与执行顺序

```
第一步（立刻）: 修复 1-A（去掉 flush）+ 修复 2（删 select）
               → 改动最小，5 分钟内完成，预期 QPS: 400 → 1500+

第二步（当天）: 修复 1-B（异步日志）
               → 需要新增异步队列类，预期 QPS: 1500 → 5000+

第三步（近期）: 修复 3（Keep-Alive）
               → 需要改 handleClient 循环逻辑，预期 QPS: 5000 → 15000+

第四步（优化）: 修复 4 + 修复 5
               → 锦上添花，进一步提升高并发稳定性
```

---

## 验证方式

```bash
# 压测命令（10线程 × 100并发，30秒）
wrk -t10 -c100 -d30s http://localhost:8080/index.html

# 修复前预期：~400 RPS
# 修复 1+2 后预期：>1500 RPS
# 全部修复后预期：>10000 RPS
```
