#include "../request/http_request.h"
#include <algorithm>
#include <cstring>

HttpRequest::HttpRequest()
    : m_method(METHOD_UNKNOWN), m_clientPort(0) {
}

HttpRequest::~HttpRequest() {
}

void HttpRequest::setMethod(Method method) {
    m_method = method;
}

void HttpRequest::setMethodString(const std::string& method) {
    m_method = stringToMethod(method);
}

HttpRequest::Method HttpRequest::getMethod() const {
    return m_method;
}

void HttpRequest::setUrl(const std::string& url) {
    m_url = url;
    size_t pos = url.find('?');
    if (pos != std::string::npos) {
        m_path = url.substr(0, pos);
    } else {
        m_path = url;
    }
}

std::string HttpRequest::getUrl() const {
    return m_url;
}

void HttpRequest::setPath(const std::string& path) {
    m_path = path;
}

std::string HttpRequest::getPath() const {
    return m_path;
}

void HttpRequest::addHeader(const std::string& key, const std::string& value) {
    m_headers[key] = value;
}

std::string HttpRequest::getHeader(const std::string& key) const {
    auto it = m_headers.find(key);
    if (it != m_headers.end()) {
        return it->second;
    }
    return "";
}

std::map<std::string, std::string> HttpRequest::getHeaders() const {
    return m_headers;
}

void HttpRequest::setBody(const std::string& body) {
    m_body = body;
}

std::string HttpRequest::getBody() const {
    return m_body;
}

void HttpRequest::setClientIp(const std::string& ip) {
    m_clientIp = ip;
}

std::string HttpRequest::getClientIp() const {
    return m_clientIp;
}

void HttpRequest::setClientPort(int port) {
    m_clientPort = port;
}

int HttpRequest::getClientPort() const {
    return m_clientPort;
}

void HttpRequest::clear() {
    m_method = METHOD_UNKNOWN;
    m_url.clear();
    m_path.clear();
    m_headers.clear();
    m_body.clear();
    m_clientIp.clear();
    m_clientPort = 0;
}

HttpRequest::Method HttpRequest::stringToMethod(const std::string& method) {
    if (strcasecmp(method.c_str(), "GET") == 0) return METHOD_GET;
    if (strcasecmp(method.c_str(), "POST") == 0) return METHOD_POST;
    if (strcasecmp(method.c_str(), "PUT") == 0) return METHOD_PUT;
    if (strcasecmp(method.c_str(), "DELETE") == 0) return METHOD_DELETE;
    return METHOD_UNKNOWN;
}

std::string HttpRequest::methodToString(Method method) {
    switch (method) {
        case METHOD_GET: return "GET";
        case METHOD_POST: return "POST";
        case METHOD_PUT: return "PUT";
        case METHOD_DELETE: return "DELETE";
        default: return "UNKNOWN";
    }
}
