#pragma once

#include <Arduino.h>
#include <FS.h>
#include <functional>
#include <memory>
#include <vector>
#include <utility>

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

    struct Cookie
    {
        enum SameSite
        {
            None,
            Lax,
            Strict
        };

        String name;
        String value;
        String path = "/";
        String domain;
        int maxAge = -1; // <0 -> session cookie
        bool httpOnly = true;
        bool secure = false;
        SameSite sameSite = Lax;
    };

    class Request;
    class Response;
    class StaticInputStream;

    using TemplateHandler = std::function<bool(const String &key, Print &out)>;
    using StaticHandler = std::function<void(const StaticInfo &info, Request &req, Response &res)>;
    using RouteHandler = std::function<void(Request &req, Response &res)>;
    using ErrorRenderer = std::function<void(int status, Request &req, Response &res)>;

    // en: Lightweight holder for esp_http_server request data.
    // ja: esp_http_server のリクエスト情報を扱う薄いラッパークラス。
    class Request
    {
    public:
        explicit Request(httpd_req_t *raw = nullptr) : _raw(raw) {}

        httpd_req_t *raw() const { return _raw; }
        String uri() const;
        String method() const;
        const String &path() const { return _normalizedPath; }
        String pathParam(const String &key) const;
        bool hasPathParam(const String &key) const;
        bool hasCookie(const String &name) const;
        String cookie(const String &name) const;
        void forEachCookie(std::function<bool(const String &name, const String &value)> cb) const;
        bool hasQueryParam(const String &name) const;
        String queryParam(const String &name) const;
        void forEachQueryParam(std::function<bool(const String &name, const String &value)> cb) const;

        bool hasFormParam(const String &name) const;
        String formParam(const String &name) const;
        void forEachFormParam(std::function<bool(const String &name, const String &value)> cb) const;
        static void setMaxFormSize(size_t bytes);

        struct MultipartFieldInfo
        {
            String name;
            String filename;
            String contentType;
            size_t size = 0; // 0 if unknown
        };

        using MultipartFieldHandler = std::function<bool(const MultipartFieldInfo &info, Stream &content)>;

        bool hasMultipartField(const String &name) const;
        String multipartField(const String &name) const;
        void onMultipart(MultipartFieldHandler handler) const;

    private:
        friend class Server;

        void setPathInfo(const String &path, const std::vector<std::pair<String, String>> &params);
        void clearPathInfo();
        bool ensureCookiesParsed() const;
        bool ensureQueryParsed() const;
        bool ensureFormParsed() const;
        bool ensureMultipartParsed() const;
        bool parseUrlEncoded(const String &text, std::vector<std::pair<String, String>> &out) const;
        static bool decodeComponent(const String &input, String &output);
        static bool isUrlEncodedContentType(const String &contentType);
        static bool extractBoundary(const String &contentType, String &boundaryOut);

        httpd_req_t *_raw = nullptr;
        String _normalizedPath = "/";
        std::vector<std::pair<String, String>> _pathParams;
        mutable bool _cookiesParsed = false;
        mutable std::vector<std::pair<String, String>> _cookies;
        mutable bool _queryParsed = false;
        mutable std::vector<std::pair<String, String>> _queryParams;
        mutable bool _formParsed = false;
        mutable bool _formOverflow = false;
        mutable std::vector<std::pair<String, String>> _formParams;
        mutable bool _multipartParsed = false;
        mutable bool _multipartOverflow = false;
        struct MultipartField
        {
            MultipartFieldInfo info;
            String data;
        };
        mutable std::vector<MultipartField> _multipartFields;
        static size_t _maxFormSize;
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
        void sendError(int status);
        bool committed() const;

        void redirect(const char *location, int status = 302);

        static void setErrorRenderer(ErrorRenderer handler);
        static void clearErrorRenderer();

        void setStaticInfo(const StaticInfo &info);

        void setCookie(const Cookie &cookie);
        void clearCookie(const String &name, const String &path = "/");

    private:
        friend class Server;

        enum class StaticSourceType
        {
            None,
            FileSystem,
            Memory
        };

        void setStaticFileSystem(fs::FS *fs);
        void setStaticMemorySource(const uint8_t *data, size_t size);
        void clearStaticSource();
        bool streamHtmlFromSource(StaticInputStream &stream);
        const char *statusString(int code);
        void setRequestContext(Request *req);
        void markCommitted();
        static const char *defaultErrorMessage(int status);

        httpd_req_t *_raw = nullptr;
        TemplateHandler _templateHandler;
        String _headInjection;
        const char *_headInjectionPtr = nullptr;
        bool _headInjectionIsRawPtr = false;
        Request *_requestContext = nullptr;
        StaticInfo _staticInfo;
        bool _chunked = false;
        int _lastStatusCode = 0;
        bool _responseCommitted = false;
        StaticSourceType _staticSource = StaticSourceType::None;
        fs::FS *_staticFs = nullptr;
        const uint8_t *_memData = nullptr;
        size_t _memSize = 0;
        std::vector<std::unique_ptr<char[]>> _setCookieBuffers;
        char _statusBuffer[16] = {0};
        static ErrorRenderer _errorRenderer;
    };

    struct SessionConfig
    {
        String cookieName = "sid";
        int maxAgeSeconds = 7 * 24 * 3600; // <0 -> session cookie
        String path = "/";
        bool secure = false;
        bool httpOnly = true;
        Cookie::SameSite sameSite = Cookie::SameSite::Lax;
        size_t idBytes = 16; // 128-bit
        std::function<String()> generate;
        std::function<bool(const String &)> validate;
        std::function<void(const String &oldId, const String &newId)> onRotate;
    };

    struct SessionInfo
    {
        String id;
        bool isNew = false;
        bool rotated = false;
    };

    SessionInfo beginSession(Request &req, Response &res, const SessionConfig &cfg);
    SessionInfo rotateSession(SessionInfo &cur, Response &res, const SessionConfig &cfg);
    void touchSessionCookie(SessionInfo &cur, Response &res, const SessionConfig &cfg);

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
        void onNotFound(RouteHandler handler);

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
        enum class HandlerType
        {
            StaticFS,
            StaticMem
        };

        struct HandlerEntry
        {
            HandlerType type = HandlerType::StaticFS;
            StaticHandler staticHandler;
            String uriPrefix;
            String basePath;
            fs::FS *fs = nullptr;
            const char *const *memPaths = nullptr;
            const uint8_t *const *memData = nullptr;
            const size_t *memSizes = nullptr;
            size_t memCount = 0;
            Server *owner = nullptr;
        };

        struct RouteSegment
        {
            enum class Type
            {
                Literal,
                Param,
                Wildcard
            };

            Type type = Type::Literal;
            String value;
        };

        struct DynamicRoute
        {
            httpd_method_t method = HTTP_GET;
            String pattern;
            std::vector<RouteSegment> segments;
            int score = 0;
            RouteHandler handler;
        };

        struct MethodHook
        {
            httpd_method_t method = HTTP_GET;
            httpd_uri_t uriDef{};
            bool registered = false;
        };

        static esp_err_t handleDynamicHttpRequest(httpd_req_t *req);
        void setupStaticInfoFromFS(HandlerEntry *entry, Request &req, Response &res, const String &normalizedUri, const String &relPath);
        void setupStaticInfoFromMemory(HandlerEntry *entry, Request &req, Response &res, const String &normalizedUri, const String &relPath);
        bool ensureMethodHook(httpd_method_t method);
        bool registerMethodHook(MethodHook *hook);
        esp_err_t dispatchDynamic(httpd_req_t *req);
        bool tryHandleStaticRequest(Request &req, Response &res, httpd_method_t method, const String &rawPath, const String &normalizedPath);
        bool parseRoutePattern(const String &pattern, std::vector<RouteSegment> &segments, int &score);
        bool normalizeRoutePath(const String &raw, String &normalized, std::vector<String> &segments) const;
        bool matchRoute(const DynamicRoute &route, const std::vector<String> &pathSegments, std::vector<std::pair<String, String>> &outParams) const;
        String urlDecode(const String &input) const;
        void logParams(const std::vector<std::pair<String, String>> &params) const;
        String clientAddress(httpd_req_t *req) const;

        httpd_handle_t _handle = nullptr;
        std::vector<std::unique_ptr<HandlerEntry>> _handlers;
        std::vector<DynamicRoute> _dynamicRoutes;
        std::vector<std::unique_ptr<MethodHook>> _methodHooks;
        RouteHandler _notFoundHandler;
    };

} // namespace EspHttpServer
