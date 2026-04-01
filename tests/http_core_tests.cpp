#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "request/http_request.h"
#include "response/http_response.h"
#include "server/http_server.h"

namespace {

int g_failed = 0;
int g_run = 0;

void expectTrue(bool condition, const std::string& message) {
    ++g_run;
    if (!condition) {
        ++g_failed;
        std::cerr << "[FAIL] " << message << std::endl;
    }
}

void expectEqual(const std::string& actual, const std::string& expected, const std::string& message) {
    ++g_run;
    if (actual != expected) {
        ++g_failed;
        std::cerr << "[FAIL] " << message << "\n  expected: " << expected << "\n  actual:   " << actual << std::endl;
    }
}

void expectEqualSize(std::size_t actual, std::size_t expected, const std::string& message) {
    ++g_run;
    if (actual != expected) {
        ++g_failed;
        std::cerr << "[FAIL] " << message << "\n  expected: " << expected << "\n  actual:   " << actual << std::endl;
    }
}

void testHttpRequestParsing() {
    HttpRequest request;
    request.setMethodString("get");
    request.setUrl("/search?q=hello+world&lang=zh-cn");
    request.addHeader("Content-Type", "application/x-www-form-urlencoded; charset=utf-8");
    request.setBody("name=alice+smith&city=%E5%8C%97%E4%BA%AC");
    request.setVersion("HTTP/1.1");
    request.setClientIp("127.0.0.1");
    request.setClientPort(12345);

    expectTrue(request.getMethod() == HttpRequest::METHOD_GET, "method string should map to GET");
    request.setMethodString("options");
    expectTrue(request.getMethod() == HttpRequest::METHOD_OPTIONS, "method string should map to OPTIONS");
    request.setMethodString("get");
    expectEqual(request.getUrl(), "/search?q=hello+world&lang=zh-cn", "url should be stored as-is");
    expectEqual(request.getPath(), "/search", "path should drop query string");
    expectEqual(request.getContentType(), "application/x-www-form-urlencoded; charset=utf-8", "content type should be retrievable");
    expectEqual(request.getClientIp(), "127.0.0.1", "client ip should be stored");
    expectTrue(request.getClientPort() == 12345, "client port should be stored");

    const auto queryParams = request.parseQueryParams();
    expectTrue(queryParams.size() == 2, "query params should contain two entries");
    expectEqual(queryParams.at("q"), "hello world", "query params should decode plus to space");
    expectEqual(queryParams.at("lang"), "zh-cn", "query params should preserve simple values");

    expectEqual(request.getParameter("q"), "hello world", "unified parameter lookup should read query params first");

    const auto formData = request.parseFormData();
    expectTrue(formData.size() == 2, "form data should contain two entries");
    expectEqual(formData.at("name"), "alice smith", "form data should decode plus to space");
    expectEqual(formData.at("city"), "北京", "form data should decode percent-encoded utf-8 bytes");

    request.setBody("name=alice+smith&city=%E5%8C%97%E4%BA%AC");
    expectEqual(request.getParameter("name"), "alice smith", "unified parameter lookup should read form body values");
}

void testHttpRequestJsonParsing() {
    HttpRequest request;
    request.setMethodString("post");
    request.setUrl("/api/items");
    request.addHeader("Content-Type", "application/json; charset=utf-8");
    request.setBody("{\"name\":\"widget\",\"count\":3,\"active\":true,\"meta\":{\"color\":\"blue\"}}");

    const auto jsonBody = request.parseJsonBody();
    expectTrue(jsonBody.size() == 4, "json body should contain four top-level entries");
    expectEqual(jsonBody.at("name"), "widget", "json string values should be decoded");
    expectEqual(jsonBody.at("count"), "3", "json numeric values should be preserved as text");
    expectEqual(jsonBody.at("active"), "true", "json boolean values should be preserved as text");
    expectEqual(jsonBody.at("meta"), "{\"color\":\"blue\"}", "nested json should be preserved as raw text");
    expectEqual(request.getParameter("meta"), "{\"color\":\"blue\"}", "unified parameter lookup should read json body values");
}

void testHttpRequestClear() {
    HttpRequest request;
    request.setMethodString("post");
    request.setUrl("/submit?x=1");
    request.addHeader("X-Test", "1");
    request.setBody("payload");
    request.setClientIp("10.0.0.1");
    request.setClientPort(8080);
    request.setVersion("HTTP/1.0");

    request.clear();

    expectTrue(request.getMethod() == HttpRequest::METHOD_UNKNOWN, "clear should reset method");
    expectEqual(request.getUrl(), "", "clear should reset url");
    expectEqual(request.getPath(), "", "clear should reset path");
    expectTrue(request.getHeaders().empty(), "clear should remove headers");
    expectEqual(request.getBody(), "", "clear should reset body");
    expectEqual(request.getClientIp(), "", "clear should reset client ip");
    expectTrue(request.getClientPort() == 0, "clear should reset client port");
    expectEqual(request.getVersion(), "", "clear should reset version");
}

void testHttpResponseSerialization() {
    HttpResponse response;
    response.setStatusCode(HttpResponse::STATUS_404_NOT_FOUND);
    response.setContentType("text/plain; charset=utf-8");
    response.addHeader("Connection", "keep-alive");
    response.addHeader("X-Test", "alpha");
    response.setBody("missing");

    const std::string header = response.buildHeaderString(response.getBodySize());

    expectTrue(header.find("HTTP/1.1 404 Not Found\r\n") == 0, "response should start with correct status line");
    expectTrue(header.find("Content-Type: text/plain; charset=utf-8\r\n") != std::string::npos, "response should include content type");
    expectTrue(header.find("Server: CppHttpServer/1.0\r\n") != std::string::npos, "response should include server header");
    expectTrue(header.find("X-Test: alpha\r\n") != std::string::npos, "response should include custom headers");
    expectTrue(header.find("Connection: keep-alive\r\n") != std::string::npos, "response should include keep-alive connection header");
    expectTrue(header.find("Connection: Close\r\n") == std::string::npos, "response should not fall back to close when keep-alive is set");
    expectTrue(header.rfind("\r\n\r\n") == header.size() - 4, "response should terminate headers with a blank line");
    expectTrue(header.find("missing") == std::string::npos, "header string should not include body bytes");
}

void testHttpResponseStandardizedHelpers() {
    const HttpResponse jsonResponse = HttpResponse::jsonError(HttpResponse::STATUS_400_BAD_REQUEST, "bad \"input\"");
    const std::string header = jsonResponse.buildHeaderString(jsonResponse.getBodySize());

    expectTrue(jsonResponse.getContentType() == "application/json; charset=utf-8", "json helper should set json content type");
    expectTrue(jsonResponse.getBody().find("\\\"input\\\"") != std::string::npos, "json error body should escape quotes");
    expectTrue(header.find("Content-Type: application/json; charset=utf-8\r\n") != std::string::npos, "json helper should serialize json content type");
    expectTrue(header.find("Content-Length: ") != std::string::npos, "json helper should always serialize content length");

    HttpResponse emptyResponse;
    const std::string emptyHeader = emptyResponse.buildHeaderString(0);
    expectTrue(emptyHeader.find("Content-Length: 0\r\n") != std::string::npos, "zero-length responses should still carry content length");
}

void testHttpResponseFileBodySize() {
    const std::filesystem::path tempFile = std::filesystem::temp_directory_path() / "http_server_test_body.txt";
    {
        std::ofstream output(tempFile, std::ios::binary);
        output << "abc123";
    }

    HttpResponse response;
    response.setFilePath(tempFile.string());

    expectEqual(response.getContentType(), "text/plain", "file extension should map to text/plain");
    expectEqualSize(response.getBodySize(), 6, "file body size should match file length");

    std::filesystem::remove(tempFile);
}

void testHttpResponseErrorFactories() {
    const HttpResponse methodNotAllowed = HttpResponse::methodNotAllowed();
    const HttpResponse methodNotAllowedWithAllow = HttpResponse::methodNotAllowed("GET, HEAD, POST");
    const HttpResponse notImplemented = HttpResponse::notImplemented();

    expectTrue(methodNotAllowed.getStatusCode() == HttpResponse::STATUS_405_METHOD_NOT_ALLOWED,
               "methodNotAllowed should set 405 status");
    expectTrue(notImplemented.getStatusCode() == HttpResponse::STATUS_501_NOT_IMPLEMENTED,
               "notImplemented should set 501 status");
    expectTrue(methodNotAllowed.buildHeaderString(methodNotAllowed.getBodySize()).find("Connection: Close\r\n") != std::string::npos,
               "405 response should serialize default close connection");
    expectTrue(methodNotAllowedWithAllow.buildHeaderString(methodNotAllowedWithAllow.getBodySize()).find("Allow: GET, HEAD, POST\r\n") != std::string::npos,
               "405 response should expose Allow header when provided");
    expectTrue(notImplemented.buildHeaderString(notImplemented.getBodySize()).find("Connection: Close\r\n") != std::string::npos,
               "501 response should serialize default close connection");
}

void testHttpServerRoutePatterns() {
    HttpServer server;
    server.registerRoutePattern(HttpRequest::METHOD_GET, "/users/{id}", [](const HttpRequest& request) {
        HttpResponse response;
        response.setStatusCode(HttpResponse::STATUS_200_OK);
        response.setContentType("text/plain; charset=utf-8");
        response.setBody(request.getPathParam("id"));
        return response;
    });

    HttpRequest request;
    request.setMethodString("GET");
    request.setUrl("/users/42");
    request.setVersion("HTTP/1.1");

    const HttpResponse response = server.handleRequest(request);
    expectTrue(response.getStatusCode() == HttpResponse::STATUS_200_OK, "pattern route should return 200");
    expectEqual(response.getBody(), "42", "pattern route should expose path parameter");
    expectTrue(!response.getHeader("X-Request-Id").empty(), "middleware should attach request id");
    expectTrue(response.getHeader("X-Content-Type-Options") == "nosniff", "middleware should attach security header");
}

void testHttpServerOptionsAndAllowHeader() {
    HttpServer server;

    HttpRequest optionsRequest;
    optionsRequest.setMethodString("OPTIONS");
    optionsRequest.setUrl("/health");
    optionsRequest.setVersion("HTTP/1.1");

    const HttpResponse optionsResponse = server.handleRequest(optionsRequest);
    expectTrue(optionsResponse.getStatusCode() == HttpResponse::STATUS_204_NO_CONTENT,
               "OPTIONS should return no-content response");
    expectTrue(optionsResponse.getHeader("Allow").find("OPTIONS") != std::string::npos,
               "OPTIONS response should advertise OPTIONS support");

    HttpRequest putRequest;
    putRequest.setMethodString("PUT");
    putRequest.setUrl("/health");
    putRequest.setVersion("HTTP/1.1");

    const HttpResponse putResponse = server.handleRequest(putRequest);
    expectTrue(putResponse.getStatusCode() == HttpResponse::STATUS_405_METHOD_NOT_ALLOWED,
               "unsupported method should return 405");
    expectTrue(putResponse.getHeader("Allow").find("OPTIONS") != std::string::npos,
               "Allow header should include OPTIONS");
}

}  // namespace

int main() {
    testHttpRequestParsing();
    testHttpRequestJsonParsing();
    testHttpRequestClear();
    testHttpResponseSerialization();
    testHttpResponseStandardizedHelpers();
    testHttpResponseFileBodySize();
    testHttpResponseErrorFactories();
    testHttpServerRoutePatterns();
    testHttpServerOptionsAndAllowHeader();

    if (g_failed != 0) {
        std::cerr << "[RESULT] " << g_failed << "/" << g_run << " checks failed" << std::endl;
        return 1;
    }

    std::cout << "[RESULT] all " << g_run << " checks passed" << std::endl;
    return 0;
}