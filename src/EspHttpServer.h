#pragma once

#include <Arduino.h>
#include <FS.h>
#include <functional>
#include <memory>
#include <vector>

extern "C"
{
#include <esp_http_server.h>
}

namespace EspHttpServer
{

    // en: Interface skeleton mirroring SPEC.md so implementation can be filled incrementally.
    // ja: SPEC.md に沿ったインターフェース骨組みで、後から徐々に実装を追加できます。

    // en: Data extracted from serveStatic so Response can stream assets.
    // ja: serveStatic で解析した静的ファイル情報を保持する構造体。
    struct StaticInfo
    {
        String uri;
        String relPath;
        String fsPath;
        bool exists = false;
        bool isDir = false;
        bool isGzipped = false;
        String logicalPath;
    };

    class Request;
    class Response;

    using TemplateHandler = std::function<bool(const String &key, Print &out)>;
    using StaticHandler = std::function<void(const StaticInfo &info, Request &req, Response &res)>;
    using RouteHandler = std::function<void(Request &req, Response &res)>;

    // en: Lightweight holder for esp_http_server request data.
    // ja: esp_http_server のリクエスト情報を扱う薄いラッパークラス。
    class Request
    {
    public:
        explicit Request(httpd_req_t *raw = nullptr) : _raw(raw) {}

        httpd_req_t *raw() const { return _raw; }
        String uri() const;
        String method() const;

    private:
        httpd_req_t *_raw = nullptr;
    };

    // en: Response facade implementing the high-level API from SPEC.md.
    // ja: SPEC.md で定義された高レベル API を提供するレスポンスクラス。
    class Response
    {
    public:
        explicit Response(httpd_req_t *raw = nullptr);

        void attachRequest(httpd_req_t *raw);

        void setTemplateHandler(TemplateHandler handler);
        void clearTemplateHandler();

        void setHeadInjection(const char *snippet);
        void setHeadInjection(const String &snippet);
        void clearHeadInjection();

        void send(int code, const char *type, const uint8_t *data, size_t len);
        void send(int code, const char *type, const String &body);
        void sendText(int code, const char *type, const char *text);
        void sendText(int code, const char *type, const String &text);

        void beginChunked(int code, const char *type);
        void sendChunk(const uint8_t *data, size_t len);
        void sendChunk(const char *text);
        void sendChunk(const String &text);
        void endChunked();

        void sendStatic();
        void sendFile(fs::FS &fs, const String &fsPath);

        void redirect(const char *location, int status = 302);

        void setStaticInfo(const StaticInfo &info);

    private:
        httpd_req_t *_raw = nullptr;
        TemplateHandler _templateHandler;
        String _headInjection;
        const char *_headInjectionPtr = nullptr;
        bool _headInjectionIsRawPtr = false;
        StaticInfo _staticInfo;
        bool _chunked = false;
    };

    // en: Minimal server wrapper coordinating route and static registrations.
    // ja: ルートと静的ハンドラを束ねる最小限のサーバークラス。
    class Server
    {
    public:
        Server();
        ~Server();

        bool begin(const httpd_config_t &cfg = HTTPD_DEFAULT_CONFIG());
        void end();

        void on(const String &uri, httpd_method_t method, RouteHandler handler);

        void serveStatic(const String &uriPrefix,
                         fs::FS &fs,
                         const String &basePath,
                         StaticHandler handler);

        void serveStatic(const String &uriPrefix,
                         const char *const *paths,
                         const uint8_t *const *data,
                         const size_t *sizes,
                         size_t fileCount,
                         StaticHandler handler);

    private:
        struct HandlerEntry
        {
            String uri;
            httpd_method_t method;
            RouteHandler handler;
        };

        httpd_handle_t _handle = nullptr;
        std::vector<HandlerEntry> _handlers;
    };

} // namespace EspHttpServer
