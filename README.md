# CppHttpServer

一个基于 C++17 的轻量级 HTTP 服务器，使用 `epoll` + 线程池模型，支持静态文件服务、目录浏览、基础 POST 请求处理与可配置日志。

## 特性

- C++17 实现，使用 CMake 构建
- 多线程并发处理（线程池）
- 基于 `epoll` 的事件驱动网络模型
- 静态文件服务（默认目录为 `html_docs/`）
- 支持目录访问时的文件列表展示
- 支持 GET/POST 基础请求处理
- 可通过配置文件与命令行参数进行运行时配置
- 支持可选 HTTPS/TLS，包含证书、私钥和密码套件配置
- 支持信号优雅退出（`SIGINT`/`SIGTERM`）

## 项目结构

```text
.
├── CMakeLists.txt
├── http_server.conf
├── src/
│   ├── main/        # 程序入口与参数解析
│   ├── server/      # 服务器核心逻辑与 epoll 管理
│   ├── request/     # HTTP 请求解析
│   ├── response/    # HTTP 响应构建
│   ├── thread/      # 线程池
│   ├── logger/      # 日志模块
│   └── cache/       # 文件缓存
├── html_docs/       # 默认静态资源目录
├── build/logs/      # 运行时日志目录
├── scripts/         # 启停与清理脚本
└── docs/            # 部署与性能文档
```

## 环境要求

- Linux / Unix
- GCC 7+ 或 Clang 5+
- CMake 3.10+
- `make`

Debian/Ubuntu 安装依赖：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake
```

## 构建

在项目根目录执行：

```bash
mkdir -p build
cd build
cmake ..
make -j"$(nproc)"
```

构建完成后，可执行文件位于：

```text
build/http_server_cpp
```

## Docker 部署

仓库根目录已经提供 `Dockerfile`，可以直接构建镜像并启动容器：

```bash
docker build -t cpp-http-server .
docker run -d --name cpp-http-server \
	-p 8080:8080 \
	-v cpp_http_logs:/app/build/logs \
	cpp-http-server
```

启动后访问：`http://localhost:8080/`

如果需要自定义端口，可以把容器内监听端口和宿主机映射一起改掉：

```bash
docker run -d --name cpp-http-server \
	-p 8081:8081 \
	-v cpp_http_logs:/app/build/logs \
	cpp-http-server ./http_server_cpp -c ./http_server.conf -p 8081
```

说明：

- 容器工作目录是 `/app/build`
- 容器内配置文件默认位于 `/app/build/http_server.conf`
- 静态资源目录默认位于 `/app/build/html_docs`
- 日志默认写入 `/app/build/logs/`
- `-v cpp_http_logs:/app/build/logs` 是推荐方式；如果你想挂载宿主机目录，先确保该目录对容器内 UID 10001 可写

也可以直接使用仓库根目录提供的 `docker-compose.yml` 启动，便于统一端口和日志卷配置：

执行命令：`docker compose up -d`

## 运行

### 方式 1：直接运行构建产物（推荐）

```bash
./build/http_server_cpp -c ./http_server.conf
```

### 方式 2：使用启动脚本

`scripts/start.sh` 会优先使用 `build/http_server_cpp`，如果不存在再回退到项目根目录下的 `http_server_cpp`：

```bash
./scripts/start.sh -p 8080 -c ./http_server.conf
```

### 方式 3：一键编译并运行（按配置文件）

```bash
./scripts/build_and_run.sh
```

默认行为：后台运行，命令会立即返回并输出 PID 与日志路径。

可选参数：

```bash
./scripts/build_and_run.sh --clean
./scripts/build_and_run.sh -c ./http_server.conf
./scripts/build_and_run.sh --foreground
```

## 配置说明

默认配置文件：`http_server.conf`

示例：

```conf
port 8080
thread_pool_size 8
doc_root ./html_docs
debug 0
log_to_console 0
```

字段说明：

- `port`：监听端口（1-65535）
- `thread_pool_size`：线程池大小
- `doc_root`：静态文件根目录
- `debug`：调试模式（1 开启，0 关闭）
- `log_to_console`：日志是否输出到控制台（1 是，0 否）
- `enable_tls`：是否启用 TLS/HTTPS（1 开启，0 关闭）
- `tls_cert_file`：TLS 证书文件路径
- `tls_key_file`：TLS 私钥文件路径
- `tls_cipher_suites`：TLS 密码套件列表

优先级：命令行参数 > 配置文件 > 默认值。

说明：TLS 相关路径会按配置文件所在目录解析，便于和配置一起部署。

## 命令行参数

```bash
./build/http_server_cpp [OPTIONS]
```

可用参数：

- `-c, --config FILE`：指定配置文件（默认 `./http_server.conf`）
- `-p, --port PORT`：指定端口
- `-d, --doc-root DIR`：指定静态目录
- `-t, --threads NUM`：指定线程数
- `-S, --tls`：启用 TLS/HTTPS
- `-C, --tls-cert FILE`：指定 TLS 证书文件
- `-K, --tls-key FILE`：指定 TLS 私钥文件
- `-Y, --tls-ciphers LIST`：指定 TLS 密码套件列表
- `-h, --help`：查看帮助
- `-v, --version`：查看版本

## 快速验证

启动服务后在另一个终端测试：

```bash
curl -i http://127.0.0.1:8080/
curl -i http://127.0.0.1:8080/index.html
```

如果启用了 TLS，可以使用 `https` 和 `-k` 验证自签名证书：

```bash
curl -k -i https://127.0.0.1:8080/
```

## 压测

项目内包含 `wrk` 工具（`scripts/wrk`），示例：

```bash
./scripts/wrk -t8 -c40 -d10s http://127.0.0.1:8080/index.html
```

## 日志

默认日志目录为 `build/logs/`，无论从哪里启动，运行时日志都会统一写入这里。常见文件：

- `build/logs/access.log`
- `build/logs/error.log`

## 常见问题

1. 端口被占用

```bash
ss -tan | grep ':8080 '
./scripts/cleanup.sh
```

2. 页面返回 404

- 确认 `doc_root` 指向正确目录
- 确认目标文件存在于 `html_docs/` 或其子目录

3. 启动脚本提示找不到二进制

- 将构建产物复制到根目录：`cp build/http_server_cpp ./http_server_cpp`
- 或直接使用 `./build/http_server_cpp` 启动

## 相关文档

- `docs/DEPLOYMENT.md`
- `docs/DEVELOPMENT_PLAN.md`
- `docs/PERFORMANCE_FIX.md`
