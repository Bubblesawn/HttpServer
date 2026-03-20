#include "../server/http_server.h"
#include "../request/http_request.h"
#include "../response/http_response.h"
#include "../thread/thread_pool.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <iostream>

HttpServer::HttpServer(const std::string& ip, int port)
    : m_ip(ip)
    , m_port(port)
    , m_docRoot("./html_docs")
    , m_numThreads(4)
    , m_serverSocket(-1)
    , m_running(false) {
    signal(SIGPIPE, SIG_IGN);
}

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start() {
    if (m_running.load()) {
        return false;
    }

    m_serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverSocket < 0) {
        perror("socket");
        return false;
    }

    int opt = 1;
    if (setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
    }

    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(m_port);
    serverAddr.sin_addr.s_addr = inet_addr(m_ip.c_str());

    if (bind(m_serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("bind");
        close(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }

    if (listen(m_serverSocket, 128) < 0) {
        perror("listen");
        close(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }

    m_running.store(true);
    m_threadPool = std::make_unique<ThreadPool>(m_numThreads);

    std::cout << "Server started on " << m_ip << ":" << m_port << std::endl;
    std::cout << "Document root: " << m_docRoot << std::endl;
    std::cout << "Thread pool size: " << m_numThreads << std::endl;

    return true;
}

void HttpServer::stop() {
    if (!m_running.load()) {
        return;
    }

    m_running.store(false);

    if (m_serverSocket >= 0) {
        close(m_serverSocket);
        m_serverSocket = -1;
    }

    if (m_threadPool) {
        m_threadPool->shutdown();
        m_threadPool.reset();
    }

    std::cout << "Server stopped" << std::endl;
}

bool HttpServer::isRunning() const {
    return m_running.load();
}

void HttpServer::setPort(int port) {
    m_port = port;
}

int HttpServer::getPort() const {
    return m_port;
}

void HttpServer::setDocRoot(const std::string& docRoot) {
    m_docRoot = docRoot;
}

std::string HttpServer::getDocRoot() const {
    return m_docRoot;
}

void HttpServer::setNumThreads(int numThreads) {
    m_numThreads = numThreads;
}

int HttpServer::getNumThreads() const {
    return m_numThreads;
}

void HttpServer::setRequestHandler(RequestHandler handler) {
    m_requestHandler = handler;
}

std::string HttpServer::getLocalIp() const {
    char ip[INET_ADDRSTRLEN];
    struct sockaddr_in addr;
    socklen_t addrLen = sizeof(addr);
    if (getsockname(m_serverSocket, (struct sockaddr*)&addr, &addrLen) == 0) {
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        return std::string(ip);
    }
    return "0.0.0.0";
}

void HttpServer::acceptConnections() {
    while (m_running.load()) {
        struct sockaddr_in clientAddr;
        socklen_t clientAddrLen = sizeof(clientAddr);

        int clientSocket = accept(m_serverSocket, (struct sockaddr*)&clientAddr, &clientAddrLen);
        if (clientSocket < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }

        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, sizeof(clientIp));
        int clientPort = ntohs(clientAddr.sin_port);

        std::cout << "Client connected: " << clientIp << ":" << clientPort << std::endl;

        m_threadPool->enqueue([this, clientSocket, clientIp, clientPort]() {
            handleClient(clientSocket, clientIp, clientPort);
        });
    }
}

void HttpServer::handleClient(int clientSocket, const std::string& clientIp, int clientPort) {
    HttpRequest request;
    request.setClientIp(clientIp);
    request.setClientPort(clientPort);

    HttpResponse response;

    try {
        request = parseRequest(clientSocket);

        if (m_requestHandler) {
            response = m_requestHandler(request);
        } else {
            if (request.getMethod() == HttpRequest::METHOD_GET) {
                response = handleStaticFile(request);
            } else {
                response = HttpResponse::notImplemented();
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Request handling exception: " << e.what() << std::endl;
        response = HttpResponse::internalServerError();
    }

    std::string responseStr = response.toString();
    sendData(clientSocket, responseStr.c_str(), responseStr.size());

    if (!response.getFilePath().empty() && response.getStatusCode() == HttpResponse::STATUS_200_OK) {
        int fileFd = open(response.getFilePath().c_str(), O_RDONLY);
        if (fileFd >= 0) {
            char buffer[8192];
            ssize_t bytesRead;
            while ((bytesRead = read(fileFd, buffer, sizeof(buffer))) > 0) {
                sendData(clientSocket, buffer, bytesRead);
            }
            close(fileFd);
        }
    } else if (!response.getBody().empty()) {
        sendData(clientSocket, response.getBody().c_str(), response.getBody().size());
    }

    close(clientSocket);
}

HttpRequest HttpServer::parseRequest(int clientSocket) const {
    HttpRequest request;
    std::string line;

    if (readLine(clientSocket, line) <= 0) {
        return request;
    }

    std::istringstream requestLine(line);
    std::string method, url, version;
    requestLine >> method >> url >> version;

    request.setMethodString(method);
    request.setUrl(url);

    while (readLine(clientSocket, line) > 0) {
        if (line.empty()) {
            break;
        }

        size_t colonPos = line.find(':');
        if (colonPos != std::string::npos) {
            std::string key = line.substr(0, colonPos);
            std::string value = line.substr(colonPos + 1);

            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            request.addHeader(key, value);
        }
    }

    std::string contentLengthStr = request.getHeader("Content-Length");
    if (!contentLengthStr.empty()) {
        size_t contentLength = std::stoul(contentLengthStr);
        if (contentLength > 0 && contentLength <= 10 * 1024 * 1024) {
            std::string body;
            body.resize(contentLength);
            readData(clientSocket, &body[0], contentLength);
            request.setBody(body);
        }
    }

    return request;
}

HttpResponse HttpServer::handleStaticFile(const HttpRequest& request) const {
    std::string urlPath = request.getPath();

    if (isPathTraversal(urlPath)) {
        return HttpResponse::badRequest();
    }

    std::string decodedPath = urlDecode(urlPath);
    if (isPathTraversal(decodedPath)) {
        return HttpResponse::badRequest();
    }

    std::string filePath = m_docRoot + decodedPath;

    struct stat st;
    if (stat(filePath.c_str(), &st) < 0) {
        std::cerr << "File not found: " << filePath << std::endl;
        return HttpResponse::notFound();
    }

    if (S_ISDIR(st.st_mode)) {
        std::string indexPath = filePath + "/index.html";
        if (stat(indexPath.c_str(), &st) == 0) {
            filePath = indexPath;
        } else {
            return handleDirectory(filePath);
        }
    }

    HttpResponse response;
    response.setStatusCode(HttpResponse::STATUS_200_OK);
    response.setFilePath(filePath);

    return response;
}

HttpResponse HttpServer::handleDirectory(const std::string& dirPath) const {
    HttpResponse response;
    response.setStatusCode(HttpResponse::STATUS_200_OK);
    response.setContentType("text/html; charset=utf-8");

    std::string html = "<html><head><title>Directory Listing</title></head><body>";
    html += "<h1>Directory: " + dirPath.substr(m_docRoot.length()) + "</h1>";
    html += "<ul>";

    DIR* dir = opendir(dirPath.c_str());
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name == "." || name == "..") continue;

            std::string fullPath = dirPath + "/" + name;
            struct stat st;
            if (stat(fullPath.c_str(), &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    html += "<li><a href=\"" + name + "/\">" + name + "/</a></li>";
                } else {
                    html += "<li><a href=\"" + name + "\">" + name + "</a></li>";
                }
            }
        }
        closedir(dir);
    }

    html += "</ul></body></html>";
    response.setBody(html);

    return response;
}

std::string HttpServer::urlDecode(const std::string& path) const {
    std::string result;
    for (size_t i = 0; i < path.length(); ++i) {
        if (path[i] == '%' && i + 2 < path.length()) {
            int value;
            std::istringstream iss(path.substr(i + 1, 2));
            if (iss >> std::hex >> value) {
                result += static_cast<char>(value);
                i += 2;
            } else {
                result += path[i];
            }
        } else if (path[i] == '+') {
            result += ' ';
        } else {
            result += path[i];
        }
    }
    return result;
}

std::string HttpServer::normalizePath(const std::string& path) const {
    std::string result = path;

    size_t pos;
    while ((pos = result.find("/./")) != std::string::npos) {
        result.erase(pos, 2);
    }

    while ((pos = result.find("//")) != std::string::npos) {
        result.erase(pos, 1);
    }

    return result;
}

bool HttpServer::isPathTraversal(const std::string& path) const {
    std::string normalized = normalizePath(path);
    return normalized.find("..") != std::string::npos;
}

int HttpServer::readLine(int socket, std::string& line) const {
    line.clear();
    char ch;
    ssize_t n;

    while (true) {
        n = read(socket, &ch, 1);
        if (n <= 0) {
            return line.empty() ? -1 : line.length();
        }

        if (ch == '\n') {
            break;
        }
        if (ch != '\r') {
            line += ch;
        }
    }

    return line.length();
}

ssize_t HttpServer::readData(int socket, char* buffer, size_t size) const {
    size_t totalRead = 0;
    ssize_t n;

    while (totalRead < size) {
        n = read(socket, buffer + totalRead, size - totalRead);
        if (n <= 0) {
            break;
        }
        totalRead += n;
    }

    return totalRead;
}

ssize_t HttpServer::sendData(int socket, const char* data, size_t size) const {
    size_t totalSent = 0;
    ssize_t n;

    while (totalSent < size) {
        n = write(socket, data + totalSent, size - totalSent);
        if (n <= 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        totalSent += n;
    }

    return totalSent;
}
