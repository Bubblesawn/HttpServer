#!/bin/bash
# 清理脚本 - 终止所有 http_server_cpp 进程并释放端口

echo "=== 清理 http_server_cpp 进程 ==="

# 查找并终止所有 http_server_cpp 进程
pids=$(pgrep -f "http_server_cpp" | grep -v grep)

if [ -n "$pids" ]; then
    echo "发现以下进程需要终止:"
    echo "$pids"
    echo ""
    
    # 先尝试正常终止
    echo "尝试正常终止进程..."
    kill $pids 2>/dev/null
    sleep 1
    
    # 检查是否还有残留进程
    remaining=$(pgrep -f "http_server_cpp" | grep -v grep)
    if [ -n "$remaining" ]; then
        echo "强制终止残留进程..."
        kill -9 $remaining 2>/dev/null
        sleep 1
    fi
    
    echo "进程清理完成"
else
    echo "没有发现 http_server_cpp 进程"
fi

echo ""
echo "=== 端口状态 ==="
for port in  8081 8082 8083 8084 8085; do
    status=$(ss -tan 2>/dev/null | grep ":$port " | head -1)
    if [ -n "$status" ]; then
        echo "端口 $port: $status"
    else
        echo "端口 $port: 空闲"
    fi
done

echo ""
echo "清理完成！"
