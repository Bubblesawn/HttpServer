#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <string>
#include <memory>
#include <functional>
#include <thread>
#include <atomic>

class ThreadPool;
class HttpRequest;
class HttpResponse;

class HttpServer {
public:
    using RequestHandler = std::function<HttpResponse(const HttpRequest& request)>;

    HttpServer(const std::string& ip = "0.0.0.0", int port = 8080);
    ~HttpServer();

    bool start();
    void stop();
    bool isRunning() const;

    void setPort(int port);
    int getPort() const;

    void setDocRoot(const std::string& docRoot);
    std::string getDocRoot() const;

    void setNumThreads(int numThreads);
    int getNumThreads() const;

    void setRequestHandler(RequestHandler handler);

    std::string getLocalIp() const;

private:
    void acceptConnections();
    void handleClient(int clientSocket, const std::string& clientIp, int clientPort);

    HttpRequest parseRequest(int clientSocket) const;
    HttpResponse handleStaticFile(const HttpRequest& request) const;
    HttpResponse handleDirectory(const std::string& dirPath) const;

    std::string urlDecode(const std::string& path) const;
    std::string normalizePath(const std::string& path) const;
    bool isPathTraversal(const std::string& path) const;

    int readLine(int socket, std::string& line) const;
    ssize_t readData(int socket, char* buffer, size_t size) const;
    ssize_t sendData(int socket, const char* data, size_t size) const;

    std::string m_ip;
    int m_port;
    std::string m_docRoot;
    int m_numThreads;

    int m_serverSocket;
    std::atomic<bool> m_running;
    std::unique_ptr<ThreadPool> m_threadPool;

    RequestHandler m_requestHandler;
};

#endif
