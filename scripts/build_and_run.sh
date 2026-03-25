#!/bin/bash
# 一键编译并按 http_server.conf 启动服务器

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
DEFAULT_CONFIG="$PROJECT_ROOT/http_server.conf"
PID_FILE="$PROJECT_ROOT/logs/http_server.pid"
OUT_LOG="$PROJECT_ROOT/logs/server.out.log"

CONFIG_FILE="$DEFAULT_CONFIG"
CLEAN_BUILD=0
FOREGROUND=0

print_usage() {
    echo "用法: $0 [-c 配置文件] [--clean] [--foreground]"
    echo "  -c, --config FILE   指定配置文件 (默认: $DEFAULT_CONFIG)"
    echo "  --clean             先删除 build 目录后再重新构建"
    echo "  -f, --foreground    前台运行（默认后台运行）"
    echo "  -h, --help          显示帮助"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--config)
            if [[ $# -lt 2 ]]; then
                echo "错误: -c/--config 需要参数" >&2
                exit 1
            fi
            CONFIG_FILE="$2"
            shift 2
            ;;
        --clean)
            CLEAN_BUILD=1
            shift
            ;;
        -f|--foreground)
            FOREGROUND=1
            shift
            ;;
        -h|--help)
            print_usage
            exit 0
            ;;
        *)
            echo "错误: 未知参数 $1" >&2
            print_usage
            exit 1
            ;;
    esac
done

if [[ ! -f "$CONFIG_FILE" ]]; then
    echo "错误: 配置文件不存在: $CONFIG_FILE" >&2
    exit 1
fi

if [[ ! -f "$PROJECT_ROOT/CMakeLists.txt" ]]; then
    echo "错误: 未找到 CMakeLists.txt，当前项目目录可能不正确: $PROJECT_ROOT" >&2
    exit 1
fi

echo "=== 一键编译运行 ==="
echo "项目目录: $PROJECT_ROOT"
echo "构建目录: $BUILD_DIR"
echo "配置文件: $CONFIG_FILE"

if [[ "$CLEAN_BUILD" -eq 1 ]]; then
    echo ""
    echo "[1/3] 清理旧构建目录..."
    rm -rf "$BUILD_DIR"
fi

echo ""
echo "[2/3] 编译项目..."
mkdir -p "$BUILD_DIR"
mkdir -p "$PROJECT_ROOT/logs"
cd "$BUILD_DIR"
cmake ..
make -j"$(nproc)"

echo ""
echo "[3/3] 启动服务器..."
# 切回项目根目录，确保相对路径（如 ./logs）一致
cd "$PROJECT_ROOT"

if [[ "$FOREGROUND" -eq 1 ]]; then
    echo "前台模式：按 Ctrl+C 停止服务器"
    exec "$BUILD_DIR/http_server_cpp" -c "$CONFIG_FILE"
fi

# 默认后台模式
if [[ -f "$PID_FILE" ]]; then
    old_pid="$(cat "$PID_FILE" 2>/dev/null || true)"
    if [[ -n "$old_pid" ]] && kill -0 "$old_pid" 2>/dev/null; then
        echo "检测到已有服务在运行 (PID: $old_pid)，正在停止旧进程..."
        kill "$old_pid" 2>/dev/null || true
        sleep 1
    fi
fi

nohup "$BUILD_DIR/http_server_cpp" -c "$CONFIG_FILE" > "$OUT_LOG" 2>&1 &
new_pid=$!
echo "$new_pid" > "$PID_FILE"

echo "已后台启动，PID: $new_pid"
echo "PID 文件: $PID_FILE"
echo "运行日志: $OUT_LOG"
echo "停止命令: kill $new_pid"
