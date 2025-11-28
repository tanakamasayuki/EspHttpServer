# ESP32 WebServer Library Specification

## 0. Overview
A lightweight HTTP server library built on top of `esp_http_server` for ESP32/Arduino. It unifies dynamic routing, filesystem and in-memory static serving, templates, head injection, gzip awareness, and diagnostic logging.

---

## 1. Response API

### 1.1 Dynamic `send`
```
void send(int code, const char* type,
          const uint8_t* data, size_t len);
void send(int code, const char* type,
          const String& body);

void sendText(int code, const char* type,
              const char* text);
void sendText(int code, const char* type,
              const String& text);
```

### 1.2 Chunked transfer
```
void beginChunked(int code, const char* type);
void sendChunk(const uint8_t* data, size_t len);
void sendChunk(const char* text);
void sendChunk(const String& text);
void endChunked();
```

### 1.3 Static helpers
```
void sendStatic();                     // streams the StaticInfo prepared by serveStatic
void sendFile(fs::FS& fs, const String& fsPath);
void sendError(int status);
```

### 1.4 Redirect
```
void redirect(const char* location, int status = 302);
```
- Always sets only the status code and `Location` header. Body stays empty regardless of success/error.

### 1.5 Error rendering
```
using ErrorRenderer =
    std::function<void(int status,
                       Request& req,
                       Response& res)>;
void setErrorRenderer(ErrorRenderer handler);
void clearErrorRenderer();
```
- When `sendStatic()` or routing detects an error, it first sets the HTTP status. By default a short plain-text message ("Not Found", "Internal Server Error" …) is returned.
- Calling `sendError(status)` just sets the status and delegates to the registered ErrorRenderer; if none is registered the default short text response is sent.
- Applications can replace error pages by installing an ErrorRenderer and emitting HTML/JSON through `res.sendText()` etc.
- ErrorRenderer should not turn failures into 200 responses – status is defined by the caller.

---

## 2. Template Engine

### 2.1 Configuration
```
using TemplateHandler =
    std::function<bool(const String& key, Print& out)>;

void setTemplateHandler(TemplateHandler cb);
void clearTemplateHandler();
```

### 2.2 Behavior
- Templates run only when the Content-Type is HTML.
- Gzipped assets bypass template/head injection to avoid inflation.
- `{{key}}` outputs escaped text, `{{{key}}}` outputs raw text.
- If the handler returns `false`, the placeholder is left untouched.
- `send()` / `sendText()` also run through the streaming template + head injection pipeline when `text/html` and not gzipped.
- `sendStatic()` applies template + head injection for non-gzipped files, and streams gzipped binaries verbatim.

---

## 3. Head Injection
```
void setHeadInjection(const char* snippet);
void setHeadInjection(const String& snippet);
void clearHeadInjection();
```
- Injects the snippet immediately after the `<head>` tag for HTML responses.
- Disabled entirely for gzipped assets.

---

## 4. Static Routing: `serveStatic`

### 4.1 `StaticInfo`
```
struct StaticInfo {
    String uri;
    String relPath;
    String fsPath;
    bool   exists;
    bool   isDir;
    bool   isGzipped;
    String logicalPath;
};
```

### 4.2 `StaticHandler`
```
using StaticHandler =
    std::function<void(const StaticInfo& info,
                       Request& req,
                       Response& res)>;
```

### 4.3 Filesystem backend
```
void serveStatic(const String& uriPrefix,
                 fs::FS& fs,
                 const String& basePath,
                 StaticHandler handler);
```
Behavior:
- Remove `uriPrefix` from the URI and treat the remainder as `relPath`.
- Build `basePath + relPath` and check for `.gz` variants (unless the client explicitly asked for `.gz`).
- If `relPath` points to a directory, probe `index.html` then `index.htm` (preferring `.gz` if available).
- When no file is found, `StaticInfo.exists=false` and the handler can implement SPA fallbacks.
- Directory probes open the file to check `isDir` before resolving indexes.
- Handler receives the populated `StaticInfo` and **must call exactly one** of `sendStatic()`, `sendFile()`, `redirect()`, or `sendError()`.
- If the handler returns without sending anything, the library auto-falls back: `info.exists==true` triggers `sendStatic()`, otherwise `sendError(404)`.

### 4.4 In-memory backend
```
void serveStatic(const String& uriPrefix,
                 const char* const* paths,
                 const uint8_t* const* data,
                 const size_t* sizes,
                 size_t fileCount,
                 StaticHandler handler);
```
Behavior mirrors the FS backend:
- Match `relPath` against `paths[i]`, with `.gz` preference identical to FS.
- Detect directories by suffix `/` or presence of child paths, then probe `index.html`/`index.htm`.
- Populate `StaticInfo.fsPath` with the logical path while `setStaticMemorySource()` attaches the actual bytes.
- If the handler returns without sending, the same fallback rule applies (`sendStatic()` when `exists`, otherwise `sendError(404)`).

### 4.5 Dynamic routing: `on`
```
void on(const String& uri,
        httpd_method_t method,
        RouteHandler handler);
```
- `uri` may contain `:param` and trailing `*wildcard` segments (see §7).
- `method` uses esp_http_server enums (HTTP_GET, POST, ...).
- Incoming paths are normalized (query removed, URL-decoded, duplicate slashes collapsed).
- Segments are categorized and scored: literal +3, param +2, wildcard +1; higher score wins, ties fall back to registration order.
- `req.path()` returns the normalized path, `req.pathParam("id")` fetches params.
- If no route matches, the request is passed to `onNotFound()` (or returns 404 by default).
- If a dynamic handler exits without sending anything, the library automatically calls `sendError(500)`.

### 4.6 Catch-all handler: `onNotFound`
```
void onNotFound(RouteHandler handler);
```
- Invoked when neither `serveStatic()` nor any `on()` route handled the request.
- Perfect place for SPA fallbacks or custom error pages.
- Leaving the handler without sending triggers an automatic `sendError(404)`.

---

## 5. `sendStatic()` common behavior
1. **Gzipped files**
   - Set `Content-Encoding: gzip`.
   - Template/head injection disabled.
   - Stream the bytes without buffering the full file.
2. **Plain files**
   - Determine MIME type from extension (supports `.gz` suffix stripping).
   - Apply template/head injection only for HTML.
   - Stream using chunked transfer, never loading the whole file into RAM.

---

## 6. Memory usage policy
- Favor streaming/character-by-character processing; avoid loading entire files into `String`.
- Even for PROGMEM arrays, stream in chunks and keep buffers minimal.
- When extra processing (templates, injections) is required, design incremental pipelines instead of whole-file copies.

---

## 7. Path semantics
- **Params**: `/user/:id` captures `id` per segment (no `/`).
- **Wildcard**: `/static/*path` captures the remainder of the path in the final segment.
- **Normalization**: collapse `//` to `/`, drop trailing `/`, decode percent-escapes, strip query before matching.
- **Scoring**: literals +3, params +2, wildcards +1; highest score wins, ties resolved by registration order.
- **Request helpers**: `req.path()` (normalized path), `req.pathParam("name")`, `req.hasPathParam("name")`.

---

## 8. Logging & debug levels
| Level | Purpose                  | Example output                         |
|-------|--------------------------|----------------------------------------|
| Error | Fatal issues only        | Handler failure, missing files         |
| Info  | Normal access tracking   | Client IP, URI, HTTP method            |
| Debug | Detailed internals       | Parsed params, template substitutions  |

### 8.1 `[RESP]` log format
- All Response logs include `[RESP][subtags...] <HTTP status> ...`.
- `send()` / `sendText()` log `[RESP] 200 text/html 512 bytes` (code + type + size).
- `sendStatic()` logs `[RESP][STATIC][FS|MEM] 200 /index.html (plain) origin=/wwwroot/index.html` detailing backend and gzip.
- Default build suppresses logs (Core Debug Level = None). Raising the Core Debug Level to Error/Info/Debug enables progressively more output.

## 9. Cookies / Session

### 9.1 Cookie helpers
- Request helpers
```
bool hasCookie(const String& name) const;
String cookie(const String& name) const;
void forEachCookie(std::function<bool(const String& name,
                                      const String& value)> cb) const;
```
  - Parses the `Cookie` header once and caches it. Stops early when `cb` returns false.
- Set-Cookie
```
struct Cookie {
    String name;
    String value;
    String path = "/";
    String domain;
    int    maxAge = -1; // <0 -> session cookie
    bool   httpOnly = true;
    bool   secure   = false;
    enum SameSite { None, Lax, Strict } sameSite = Lax;
};

void setCookie(const Cookie& c);
void clearCookie(const String& name,
                 const String& path = "/"); // sends Max-Age=0
```
- Validation: drop cookies whose name/value contain control chars or newlines. Force `secure=true` when `SameSite=None`. Values stay raw (no URL decoding).
- Memory: tokenize the single Cookie line, avoid extra `String` copies. `setCookie()` may be called multiple times (headers are appended).

### 9.2 Session ID helper
- Config
```
struct SessionConfig {
    String cookieName = "sid";
    int    maxAgeSeconds = 7*24*3600; // <0 -> session cookie
    String path = "/";
    bool   secure = false;
    bool   httpOnly = true;
    Cookie::SameSite sameSite = Cookie::SameSite::Lax;
    size_t idBytes = 16; // 128-bit
    std::function<String()> generate; // default: esp_random -> base64url/hex
    std::function<bool(const String&)> validate; // return false to reject
    std::function<void(const String& oldId,
                       const String& newId)> onRotate; // optional: migration hook
};
```
- API
```
struct SessionInfo { String id; bool isNew; bool rotated; };

SessionInfo beginSession(Request& req, Response& res,
                         const SessionConfig& cfg);
SessionInfo rotateSession(SessionInfo& cur,
                          Response& res,
                          const SessionConfig& cfg);
void touchSessionCookie(SessionInfo& cur,
                        Response& res,
                        const SessionConfig& cfg);
```
- Behavior
  - `beginSession`: if the cookie exists, pass through `validate` and adopt it; otherwise issue a new ID and set `isNew=true`. When `maxAgeSeconds` is set, the same ID can be re-sent via Set-Cookie to extend TTL.
  - `rotateSession`: issues a new ID, overwrites the cookie, marks `rotated=true`, and calls `onRotate(old,new)` if provided.
  - `touchSessionCookie`: re-sends Max-Age without changing the ID (idle extension).
- Caller responsibilities: actual session storage keyed by ID, cleanup, and fixation-safe data migration belong to the application.
- Recommendations: keep `httpOnly=true`, default `sameSite=Lax`; require `secure=true` when `SameSite=None`; prefer `secure=true` under HTTPS.

## 10. Usage examples
```cpp
res.setTemplateHandler([](const String& key, Print& out){
    if (key == "name") { out.print("TANAKA"); return true; }
    return false;
});
res.setHeadInjection("<script src='/app.js'></script>");
res.sendText(200, "text/html", htmlTemplate);
```
```cpp
server.serveStatic("/view", LittleFS, "/tmpl",
  [&](const StaticInfo& info, Request& req, Response& res){
      res.setTemplateHandler(...);
      res.setHeadInjection("<script src='/app.js'></script>");
      res.sendStatic();
  });
```
```cpp
server.serveStatic("/static",
                   g_paths, g_data, g_sizes, g_fileCount,
                   [&](const StaticInfo& info, Request& req, Response& res){
      if (!info.isGzipped && info.logicalPath.endsWith(".html"))
          res.setHeadInjection("<script src='/static/app.js'></script>");
      res.sendStatic();
});
```
```cpp
if (!info.exists) {
    res.sendFile(LittleFS, "/app/index.html");
    return;
}
res.sendStatic();
```
