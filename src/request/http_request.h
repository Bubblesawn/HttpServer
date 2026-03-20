#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <string>
#include <map>
#include <memory>

class HttpRequest {
public:
    enum Method {
        METHOD_GET,
        METHOD_POST,
        METHOD_PUT,
        METHOD_DELETE,
        METHOD_UNKNOWN
    };

    HttpRequest();
    ~HttpRequest();

    void setMethod(Method method);
    void setMethodString(const std::string& method);
    Method getMethod() const;

    void setUrl(const std::string& url);
    std::string getUrl() const;

    void setPath(const std::string& path);
    std::string getPath() const;

    void addHeader(const std::string& key, const std::string& value);
    std::string getHeader(const std::string& key) const;
    std::map<std::string, std::string> getHeaders() const;

    void setBody(const std::string& body);
    std::string getBody() const;

    void setClientIp(const std::string& ip);
    std::string getClientIp() const;

    void setClientPort(int port);
    int getClientPort() const;

    void clear();

    static Method stringToMethod(const std::string& method);
    static std::string methodToString(Method method);

private:
    Method m_method;
    std::string m_url;
    std::string m_path;
    std::map<std::string, std::string> m_headers;
    std::string m_body;
    std::string m_clientIp;
    int m_clientPort;
};

#endif
