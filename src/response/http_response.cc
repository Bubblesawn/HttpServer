#include "../response/http_response.h"
#include <cstring>
#include <sys/stat.h>
#include <iostream>

HttpResponse::HttpResponse()
    : m_statusCode(STATUS_200_OK), m_statusMessage("OK") {
}

HttpResponse::~HttpResponse() {
}

void HttpResponse::setStatusCode(StatusCode code) {
    m_statusCode = code;
    m_statusMessage = statusCodeToString(code);
}

HttpResponse::StatusCode HttpResponse::getStatusCode() const {
    return m_statusCode;
}

void HttpResponse::setStatusMessage(const std::string& message) {
    m_statusMessage = message;
}

std::string HttpResponse::getStatusMessage() const {
    return m_statusMessage;
}

void HttpResponse::setContentType(const std::string& contentType) {
    m_contentType = contentType;
}

std::string HttpResponse::getContentType() const {
    return m_contentType;
}

void HttpResponse::addHeader(const std::string& key, const std::string& value) {
    m_headers[key] = value;
}

std::string HttpResponse::getHeader(const std::string& key) const {
    auto it = m_headers.find(key);
    if (it != m_headers.end()) {
        return it->second;
    }
    return "";
}

std::map<std::string, std::string> HttpResponse::getHeaders() const {
    return m_headers;
}

void HttpResponse::setBody(const std::string& body) {
    m_body = body;
    m_filePath.clear();
}

void HttpResponse::setBody(const char* data, size_t len) {
    m_body.assign(data, len);
    m_filePath.clear();
}

std::string HttpResponse::getBody() const {
    return m_body;
}

size_t HttpResponse::getBodySize() const {
    if (!m_filePath.empty()) {
        struct stat st;
        if (stat(m_filePath.c_str(), &st) == 0) {
            return st.st_size;
        }
    }
    return m_body.size();
}

void HttpResponse::setFilePath(const std::string& filepath) {
    m_filePath = filepath;
    m_contentType = getContentTypeByExtension(filepath);
}

std::string HttpResponse::getFilePath() const {
    return m_filePath;
}

std::string HttpResponse::toString() const {
    std::string result = "HTTP/1.1 " + std::to_string(m_statusCode) + " " + m_statusMessage + "\r\n";

    if (!m_contentType.empty()) {
        result += "Content-Type: " + m_contentType + "\r\n";
    }

    result += "Server: CppHttpServer/1.0\r\n";

    for (const auto& header : m_headers) {
        result += header.first + ": " + header.second + "\r\n";
    }

    if (!m_body.empty()) {
        result += "Content-Length: " + std::to_string(m_body.size()) + "\r\n";
    }

    result += "Connection: Close\r\n";
    result += "\r\n";

    return result;
}

std::string HttpResponse::statusCodeToString(StatusCode code) {
    switch (code) {
        case STATUS_200_OK: return "OK";
        case STATUS_201_CREATED: return "Created";
        case STATUS_204_NO_CONTENT: return "No Content";
        case STATUS_301_MOVED_PERMANENTLY: return "Moved Permanently";
        case STATUS_302_FOUND: return "Found";
        case STATUS_304_NOT_MODIFIED: return "Not Modified";
        case STATUS_400_BAD_REQUEST: return "Bad Request";
        case STATUS_401_UNAUTHORIZED: return "Unauthorized";
        case STATUS_403_FORBIDDEN: return "Forbidden";
        case STATUS_404_NOT_FOUND: return "Not Found";
        case STATUS_405_METHOD_NOT_ALLOWED: return "Method Not Allowed";
        case STATUS_408_REQUEST_TIMEOUT: return "Request Timeout";
        case STATUS_413_PAYLOAD_TOO_LARGE: return "Payload Too Large";
        case STATUS_414_URI_TOO_LONG: return "URI Too Long";
        case STATUS_500_INTERNAL_SERVER_ERROR: return "Internal Server Error";
        case STATUS_501_NOT_IMPLEMENTED: return "Not Implemented";
        case STATUS_502_BAD_GATEWAY: return "Bad Gateway";
        case STATUS_503_SERVICE_UNAVAILABLE: return "Service Unavailable";
        case STATUS_505_HTTP_VERSION_NOT_SUPPORTED: return "HTTP Version Not Supported";
        default: return "Unknown";
    }
}

std::string HttpResponse::getContentTypeByExtension(const std::string& path) {
    size_t pos = path.find_last_of('.');
    if (pos == std::string::npos) {
        return "text/plain";
    }

    std::string ext = path.substr(pos);
    if (ext == ".html" || ext == ".htm") return "text/html";
    if (ext == ".css") return "text/css";
    if (ext == ".js") return "application/javascript";
    if (ext == ".json") return "application/json";
    if (ext == ".xml") return "application/xml";
    if (ext == ".txt") return "text/plain";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".png") return "image/png";
    if (ext == ".gif") return "image/gif";
    if (ext == ".bmp") return "image/bmp";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".woff") return "font/woff";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".ttf") return "font/ttf";
    if (ext == ".eot") return "application/vnd.ms-fontobject";
    if (ext == ".pdf") return "application/pdf";
    if (ext == ".zip") return "application/zip";

    return "application/octet-stream";
}

HttpResponse HttpResponse::badRequest() {
    HttpResponse response;
    response.setStatusCode(STATUS_400_BAD_REQUEST);
    response.setContentType("text/html; charset=utf-8");
    response.setBody("<html><head><title>400 Bad Request</title></head>"
                     "<body><center><h1>400 Bad Request</h1></center></body></html>");
    return response;
}

HttpResponse HttpResponse::notFound() {
    HttpResponse response;
    response.setStatusCode(STATUS_404_NOT_FOUND);
    response.setContentType("text/html; charset=utf-8");
    response.setBody("<html><head><title>404 Not Found</title></head>"
                     "<body><center><h1>404 Not Found</h1></center></body></html>");
    return response;
}

HttpResponse HttpResponse::methodNotAllowed() {
    HttpResponse response;
    response.setStatusCode(STATUS_405_METHOD_NOT_ALLOWED);
    response.setContentType("text/html; charset=utf-8");
    response.setBody("<html><head><title>405 Method Not Allowed</title></head>"
                     "<body><center><h1>405 Method Not Allowed</h1></center></body></html>");
    return response;
}

HttpResponse HttpResponse::internalServerError() {
    HttpResponse response;
    response.setStatusCode(STATUS_500_INTERNAL_SERVER_ERROR);
    response.setContentType("text/html; charset=utf-8");
    response.setBody("<html><head><title>500 Internal Server Error</title></head>"
                     "<body><center><h1>500 Internal Server Error</h1></center></body></html>");
    return response;
}

HttpResponse HttpResponse::notImplemented() {
    HttpResponse response;
    response.setStatusCode(STATUS_501_NOT_IMPLEMENTED);
    response.setContentType("text/html; charset=utf-8");
    response.setBody("<html><head><title>501 Not Implemented</title></head>"
                     "<body><center><h1>501 Not Implemented</h1></center></body></html>");
    return response;
}
