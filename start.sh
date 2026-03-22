#!/bin/bash
# HTTP 服务器启动脚本

# 默认配置
PORT=8080
CONFIG="./http_server.conf"

# 解析命令行参数
while [[ $# -gt 0 ]]; do
    case $1 in
        -p|--port)
            PORT="$2"
            shift 2
            ;;
        -c|--config)
            CONFIG="$2"
            shift 2
            ;;
        *)
            echo "未知参数: $1"
            echo "用法: $0 [-p 端口] [-c 配置文件]"
            exit 1
            ;;
    esac
done

echo "=== HTTP 服务器启动脚本 ==="
echo ""

# 第一步：清理残留进程
echo "步骤 1: 清理残留进程..."
./cleanup.sh > /dev/null 2>&1

# 第二步：检查端口是否被占用
echo ""
echo "步骤 2: 检查端口 $PORT 是否可用..."
if ss -tan 2>/dev/null | grep -q ":$PORT "; then
    echo "错误: 端口 $PORT 已被占用!"
    echo "请尝试使用其他端口，或手动终止占用该端口的进程"
    exit 1
fi
echo "端口 $PORT 可用"

# 第三步：修改配置文件中的端口
echo ""
echo "步骤 3: 更新配置文件..."
if [ -f "$CONFIG" ]; then
    # 备份原配置
    cp "$CONFIG" "$CONFIG.bak"
    # 修改端口
    sed -i "s/^port .*/port $PORT/" "$CONFIG"
    echo "配置文件已更新: $CONFIG (端口: $PORT)"
else
    echo "警告: 配置文件不存在: $CONFIG"
fi

# 第四步：启动服务器
echo ""
echo "步骤 4: 启动 HTTP 服务器..."
echo "=========================================="
./http_server_cpp -c "$CONFIG"
