#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>

#define SERVER_PORT 8080

static int debug = 1;

int get_line(int sock, char *buf, int size);
void* do_http_request(void* client_sock);
void do_http_response(int client_sock, const char *path);
void header(int client_sock, FILE *resource, const char* path);
void cat(int client_sock, FILE *resource);

void not_found(int client_sock);    // 404:  文件不存在
void unimplemented(int client_sock); // 501:请求方法未实现
void bad_request(int client_sock);   // 400：请求格式错误

// 根据文件扩展名返回 Content-Type
const char* get_content_type(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return "text/plain";
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".gif") == 0) return "image/gif";
    if (strcmp(ext, ".ico") == 0) return "image/x-icon";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".txt") == 0) return "text/plain";
    return "application/octet-stream";
}

int main()
{
    int sock;
    struct sockaddr_in server_addr;

    // 让重定向到文件（nohup）时也能及时看到日志
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    // 防止对端提前断开导致 write() 触发 SIGPIPE 终止进程
    signal(SIGPIPE, SIG_IGN);

    // 创建套接字
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        perror("socket");
        return -1;
    }

    // 允许端口快速复用，避免重启时 TIME_WAIT 导致 bind 失败
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("setsockopt(SO_REUSEADDR)");
    }

    // 初始化服务器地址结构
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;                // IPv4
    server_addr.sin_port = htons(SERVER_PORT);       // 端口
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY); // 监听所有地址

    // 绑定地址
    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("bind");
        close(sock);
        return -1;
    }

    // 开始监听
    if (listen(sock, 128) < 0)
    {
        perror("listen");
        close(sock);
        return -1;
    }

    printf("Server is listening on port %d...\n", SERVER_PORT);

    int done = 1;
    while (done)
    {
        struct sockaddr_in client;
        int client_sock;
        char client_ip[64];
        pthread_t tid;
        int* pclient_sock = NULL;

        socklen_t client_addr_len = sizeof(client);
        client_sock = accept(sock, (struct sockaddr *)&client, &client_addr_len); // 接收客户端请求

        if (client_sock < 0)
        {
            perror("accept");
            continue;
        }

        // 打印客户端 IP 和端口
        printf("Client connected: %s  port：%d\n",
               inet_ntop(AF_INET, &client.sin_addr.s_addr, client_ip, sizeof(client_ip)), ntohs(client.sin_port));

        //do_http_request(client_sock); // 处理 HTTP 请求

        //启动线程处理http请求
        pclient_sock = (int*)malloc(sizeof(int));
        *pclient_sock = client_sock;
        pthread_create(&tid, NULL, do_http_request, (void*)pclient_sock);
        pthread_detach(tid); 

                
    }

    close(sock);
    return 0;
}

void* do_http_request(void* pclient_sock)
{
    if (pclient_sock == NULL) {
        return NULL;
    }
    /* 读取客户端发送的请求 */
    int len = 0;
    char buf[1024];
    char method[64]; // 请求方法
    char url[256];   // 请求 URL
    char path[512];  // 转换后的路径
    struct stat st;
    int client_sock = *(int*)pclient_sock;

    // 读取请求行
    len = get_line(client_sock, buf, sizeof(buf));

    if (len > 0)
    { // 读到了请求行
        int i = 0, j = 0;

        // 提取请求方法
        while (!isspace(buf[j]) && i < sizeof(method) - 1)
        {
            method[i++] = buf[j++];
        }
        method[i] = '\0';
        printf("request method: %s\n", method);

        // 只处理 GET 请求
        if (strcasecmp(method, "GET") == 0)
        {
            if (debug)
                printf("method = GET\n");

            // 获取 URL
            while (j < len && isspace((unsigned char)buf[j])) j++; // 跳过空格
            i = 0;
            while (j < len && !isspace((unsigned char)buf[j]) && i < (int)sizeof(url) - 1)
            {
                url[i++] = buf[j++];
            }
            url[i] = '\0';

            // 兜底：例如请求行异常导致没解析出 URL
            if (url[0] == '\0')
            {
                strcpy(url, "/");
            }

            if (debug)
                printf("url = %s\n", url);

            // 读取剩余的 HTTP 头部
            do
            {
                len = get_line(client_sock, buf, sizeof(buf));
                if (debug)
                    printf("read: %s\n", buf);
            } while (len > 0);

            // 定位服务器本地的 HTML 文件
            // 处理 URL 中的查询参数
            char *pos = strchr(url, '?');
            if (pos)
            {
                *pos = '\0'; // 截断查询参数
                printf("real url = %s\n", url);
            }

            // 拼接文件路径（url 形如 / 或 /xxx）
            snprintf(path, sizeof(path), "./html_docs%s", url);

            if (stat(path, &st) == -1)
            {
                fprintf(stderr, "file not found: %s\n", path);
                not_found(client_sock);
            }
            else
            {
                if (S_ISDIR(st.st_mode))
                {
                    strcat(path, "/index.html");
                    // 重新 stat 检查 index.html 是否存在
                    if (stat(path, &st) == -1) {
                        fprintf(stderr, "file not found: %s\n", path);
                        not_found(client_sock);
                        close(client_sock);
                        if(pclient_sock) free(pclient_sock);
                        return NULL;
                    }
                }
                do_http_response(client_sock, path); // 执行 HTTP 响应
            }
        }
        else
        { // 非 GET 请求，返回 501 Method Not Implemented
            fprintf(stderr, "warning! other request [%s]\n", method);

            // 读取剩余的 HTTP 头部
            do
            {
                len = get_line(client_sock, buf, sizeof(buf));
                if (debug)
                    printf("read: %s\n", buf);
            } while (len > 0);

            // 实现 501 响应:请求未实现
            unimplemented(client_sock);
        }
    }
    else
    { // 请求格式有问题，出错处理
        bad_request(client_sock);
    }

    close(client_sock);

    if(pclient_sock) free(pclient_sock);

    return NULL;
}

int get_line(int sock, char *buf, int size)
{
    int count = 0;
    char ch = '\0';
    int len = 0;

    // 逐字符读取，直到遇到换行符或缓冲区满
    while ((count < size - 1) && ch != '\n')
    {
        len = read(sock, &ch, 1);
        if (len == 1)
        {
            if (ch == '\r')
            {
                continue; // 忽略回车符
            }
            else if (ch == '\n')
            {
                buf[count] = '\0';
                break;
            }
            buf[count++] = ch;
        }
        else if (len == -1)
        {
            count = -1;
            perror("read failed");
            break;
        }
        else
        { // read 返回 0，客户端关闭连接
            count = -1;
            fprintf(stderr, "client closed\n");
            break;
        }
    }
    if (count >= 0)
    {
        buf[count] = '\0';
    }
    return count;
}

void do_http_response(int client_sock, const char *path)
{
    FILE *resource = fopen(path, "rb");
    if (resource == NULL)
    {
        not_found(client_sock);
        return;
    }

    header(client_sock, resource, path); // 发送 HTTP 头部
    cat(client_sock, resource);    // 发送 HTTP 响应体
    fclose(resource);
}

void not_found(int client_sock)
{
    const char *main_header = "HTTP/1.1 404 NOT FOUND\r\n"
                              "Server: Xiaoli Server\r\n"
                              "Content-Type: text/html\r\n"
                              "Connection: Close\r\n\r\n";
    const char *content = "<html><head><title>404 NOT FOUND</title></head>"
                          "<body bgcolor=\"ffffff\">\r\n"
                          "<center><h1>404 NOT FOUND</h1></center>\r\n"
                          "<hr><center>Xiaoli Server</center>\r\n"
                          "</body></html>\r\n";

    write(client_sock, main_header, strlen(main_header));
    write(client_sock, content, strlen(content));
}

void unimplemented(int client_sock)
{

    const char *reply = "HTTP/1.1 501 Method Not Implemented\r\n"
                        "Server: Xiaoli Server\r\n"
                        "Content-Type: text/html\r\n"
                        "Connection: Close\r\n\r\n";
    int len = write(client_sock, reply, strlen(reply));
    if (debug)
        fprintf(stdout,"%s" , reply);
    if (len <= 0)
    {
        fprintf(stderr, "send reply error! reason:%s", strerror(errno));
    }
}

void bad_request(int client_sock)
{
    const char *reply = "HTTP/1.1 400 Bad Request\r\n"
                         "Server: Xiaoli Server\r\n"
                         "Content-Type: text/html\r\n"
                         "Connection: Close\r\n\r\n";

    int len = write(client_sock, reply, strlen(reply));
    if (debug)
        fprintf(stdout,"%s", reply);
    if (len <= 0)
    {
        fprintf(stderr, "send reply error! reason:%s", strerror(errno));
    }
}

void header(int client_sock, FILE *resource, const char* path)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
        "HTTP/1.1 200 OK\r\n"
        "Server: Xiaoli Server\r\n"
        "Content-Type: %s\r\n"
        "Connection: Close\r\n\r\n",
        get_content_type(path));
    write(client_sock, buf, strlen(buf));
}

void cat(int client_sock, FILE *resource)
{
    unsigned char buf[8192];
    size_t nread;

    while ((nread = fread(buf, 1, sizeof(buf), resource)) > 0)
    {
        size_t total = 0;
        while (total < nread)
        {
            ssize_t nwritten = write(client_sock, buf + total, nread - total);
            if (nwritten <= 0)
            {
                fprintf(stderr, "send body error! reason:%s", strerror(errno));
                return;
            }
            total += (size_t)nwritten;
        }
    }
}

