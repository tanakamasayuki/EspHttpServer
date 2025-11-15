#include "EspHttpServer.h"

#include <esp_log.h>
#include <cstring>

namespace EspHttpServer {

namespace {
const char* TAG = "EspHttpServer";
}

// -------- Request --------

String Request::uri() const {
    return _raw ? String(_raw->uri ? _raw->uri : "") : String();
}

String Request::method() const {
    if (!_raw) return {};
    switch (_raw->method) {
        case HTTP_GET:
            return String("GET");
        case HTTP_POST:
            return String("POST");
        case HTTP_PUT:
            return String("PUT");
        case HTTP_DELETE:
            return String("DELETE");
        default:
            return String("UNKNOWN");
    }
}

// -------- Response --------

Response::Response(httpd_req_t* raw) { attachRequest(raw); }

void Response::attachRequest(httpd_req_t* raw) {
    _raw = raw;
    _chunked = false;
}

void Response::setTemplateHandler(TemplateHandler handler) {
    _templateHandler = std::move(handler);
}

void Response::clearTemplateHandler() {
    _templateHandler = nullptr;
}

void Response::setHeadInjection(const char* snippet) {
    _headInjectionPtr = snippet;
    _headInjectionIsRawPtr = true;
    _headInjection.clear();
}

void Response::setHeadInjection(const String& snippet) {
    _headInjection = snippet;
    _headInjectionPtr = _headInjection.c_str();
    _headInjectionIsRawPtr = false;
}

void Response::clearHeadInjection() {
    _headInjectionPtr = nullptr;
    _headInjection.clear();
    _headInjectionIsRawPtr = false;
}

void Response::send(int code, const char* type, const uint8_t* data, size_t len) {
    if (!_raw) return;
    httpd_resp_set_type(_raw, type);
    httpd_resp_set_status(_raw, String(code).c_str());
    httpd_resp_send(_raw, reinterpret_cast<const char*>(data), len);
}

void Response::send(int code, const char* type, const String& body) {
    send(code, type, reinterpret_cast<const uint8_t*>(body.c_str()), body.length());
}

void Response::sendText(int code, const char* type, const char* text) {
    send(code, type, reinterpret_cast<const uint8_t*>(text), strlen(text));
}

void Response::sendText(int code, const char* type, const String& text) {
    send(code, type, reinterpret_cast<const uint8_t*>(text.c_str()), text.length());
}

void Response::beginChunked(int code, const char* type) {
    if (!_raw) return;
    _chunked = true;
    httpd_resp_set_type(_raw, type);
    httpd_resp_set_status(_raw, String(code).c_str());
}

void Response::sendChunk(const uint8_t* data, size_t len) {
    if (!_raw || !_chunked) return;
    httpd_resp_send_chunk(_raw, reinterpret_cast<const char*>(data), len);
}

void Response::sendChunk(const char* text) {
    sendChunk(reinterpret_cast<const uint8_t*>(text), strlen(text));
}

void Response::sendChunk(const String& text) {
    sendChunk(reinterpret_cast<const uint8_t*>(text.c_str()), text.length());
}

void Response::endChunked() {
    if (!_raw || !_chunked) return;
    httpd_resp_send_chunk(_raw, nullptr, 0);
    _chunked = false;
}

void Response::sendStatic() {
    // en: Placeholder – real implementation will stream from FS or memory context.
    // ja: プレースホルダー – 実装時に FS/メモリバンドルからストリーム送信します。
    ESP_LOGW(TAG, "sendStatic() is not yet implemented");
    httpd_resp_send_500(_raw);
}

void Response::sendFile(fs::FS& fs, const String& fsPath) {
    // en: Placeholder streaming logic will be added in later phases.
    // ja: 後続フェーズで実際のストリーム処理を実装します。
    (void)fs;
    (void)fsPath;
    ESP_LOGW(TAG, "sendFile(%s) not implemented", fsPath.c_str());
    httpd_resp_send_500(_raw);
}

void Response::redirect(const char* location, int status) {
    if (!_raw) return;
    httpd_resp_set_status(_raw, String(status).c_str());
    httpd_resp_set_hdr(_raw, "Location", location);
    httpd_resp_send(_raw, nullptr, 0);
}

void Response::setStaticInfo(const StaticInfo& info) {
    _staticInfo = info;
}

// -------- Server --------

Server::Server() = default;
Server::~Server() { end(); }

bool Server::begin(const httpd_config_t& cfg) {
    if (_handle) return true;
    esp_err_t err = httpd_start(&_handle, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void Server::end() {
    if (_handle) {
        httpd_stop(_handle);
        _handle = nullptr;
    }
}

void Server::on(const String& uri, httpd_method_t method, RouteHandler handler) {
    HandlerEntry entry{uri, method, std::move(handler)};
    _handlers.push_back(std::move(entry));
    // en: Actual registration with esp_http_server will happen once implementation matures.
    // ja: esp_http_server への登録は実装フェーズで追加予定です。
}

void Server::serveStatic(const String& uriPrefix,
                         fs::FS& fs,
                         const String& basePath,
                         StaticHandler handler) {
    (void)uriPrefix;
    (void)fs;
    (void)basePath;
    (void)handler;
    ESP_LOGW(TAG, "serveStatic(FS) stub");
}

void Server::serveStatic(const String& uriPrefix,
                         const char* const* paths,
                         const uint8_t* const* data,
                         const size_t* sizes,
                         size_t fileCount,
                         StaticHandler handler) {
    (void)uriPrefix;
    (void)paths;
    (void)data;
    (void)sizes;
    (void)fileCount;
    (void)handler;
    ESP_LOGW(TAG, "serveStatic(MemFS) stub");
}

}  // namespace EspHttpServer
