#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <string>
#include <map>
#include <memory>
#include <cstdint>

class HttpResponse {
public:
    enum StatusCode {
        STATUS_200_OK = 200,
        STATUS_201_CREATED = 201,
        STATUS_204_NO_CONTENT = 204,
        STATUS_301_MOVED_PERMANENTLY = 301,
        STATUS_302_FOUND = 302,
        STATUS_304_NOT_MODIFIED = 304,
        STATUS_400_BAD_REQUEST = 400,
        STATUS_401_UNAUTHORIZED = 401,
        STATUS_403_FORBIDDEN = 403,
        STATUS_404_NOT_FOUND = 404,
        STATUS_405_METHOD_NOT_ALLOWED = 405,
        STATUS_408_REQUEST_TIMEOUT = 408,
        STATUS_413_PAYLOAD_TOO_LARGE = 413,
        STATUS_414_URI_TOO_LONG = 414,
        STATUS_500_INTERNAL_SERVER_ERROR = 500,
        STATUS_501_NOT_IMPLEMENTED = 501,
        STATUS_502_BAD_GATEWAY = 502,
        STATUS_503_SERVICE_UNAVAILABLE = 503,
        STATUS_505_HTTP_VERSION_NOT_SUPPORTED = 505
    };

    HttpResponse();
    ~HttpResponse();

    void setStatusCode(StatusCode code);
    StatusCode getStatusCode() const;

    void setStatusMessage(const std::string& message);
    std::string getStatusMessage() const;

    void setContentType(const std::string& contentType);
    std::string getContentType() const;

    void addHeader(const std::string& key, const std::string& value);
    std::string getHeader(const std::string& key) const;
    std::map<std::string, std::string> getHeaders() const;

    void setBody(const std::string& body);
    void setBody(const char* data, size_t len);
    std::string getBody() const;
    size_t getBodySize() const;

    void setFilePath(const std::string& filepath);
    std::string getFilePath() const;

    std::string toString() const;

    static std::string statusCodeToString(StatusCode code);
    static std::string getContentTypeByExtension(const std::string& path);

    static HttpResponse badRequest();
    static HttpResponse notFound();
    static HttpResponse methodNotAllowed();
    static HttpResponse internalServerError();
    static HttpResponse notImplemented();

private:
    StatusCode m_statusCode;
    std::string m_statusMessage;
    std::string m_contentType;
    std::map<std::string, std::string> m_headers;
    std::string m_body;
    std::string m_filePath;
};

#endif
