# ESP32 HTTP Server Library

[日本語 README](README.ja.md)

ESP32 向けに `esp_http_server` をベースとして構築する軽量な Arduino ライブラリの仕様まとめです。動的レスポンス、テンプレート、head injection、gzip を含む静的ファイル配信など、Web アプリで必要となる機能をワンストップで提供することを目指しています。

## Highlights

- **Response API** – Direct send helpers, chunked transfer, static streaming, and HTTP redirects. Designed around `Request`/`Response` handlers similar to popular Arduino servers.
- **Template Engine** – HTML-only placeholder replacement (`{{key}}` escaped, `{{{key}}}` raw) with user-provided callbacks. Automatically disabled for gzipped assets.
- **Head Injection** – Optional snippet inserted immediately after `<head>` when serving HTML, enabling CSP tags, scripts, or analytics without manual template edits.
- **Static Serving** – `serveStatic` for filesystem backends (SPIFFS/LittleFS/etc.) and in-memory bundles. Supports gzip preference, directory metadata, and SPA fallbacks.
- **Gzip Awareness** – Static responses detect `.gz` pairs, set `Content-Encoding`, and bypass template/head injection for maximal performance.

See [SPEC.md](SPEC.md) for the complete technical requirements.

## Components

### Response Helpers
```cpp
void send(int code, const char* type,
          const uint8_t* data, size_t len);
void send(int code, const char* type,
          const String& body);
void beginChunked(int code, const char* type);
void sendChunk(const uint8_t* data, size_t len);
void sendStatic();
void sendFile(fs::FS& fs, const String& fsPath);
void redirect(const char* location, int status = 302);
```

These APIs wrap `esp_http_server` to simplify dynamic text/binary responses, chunked streaming, and static file reads. `sendStatic()` relies on a `StaticInfo` context prepared by `serveStatic`.

### Template Engine & Head Injection

Applications can call `setTemplateHandler` with a `std::function` that receives placeholder keys and a `Print` instance for emission. Head injection stores either a `const char*` or `String` snippet and applies it only when a non-gzipped HTML response passes through the renderer.

### Static Routing

Two overloads of `serveStatic` populate a `StaticInfo` struct and invoke a user handler:

- **Filesystem backend** – Resolves URI prefix to `basePath`, checks for `.gz` variants, and exposes metadata such as `exists`, `isDir`, and `logicalPath`.
- **Memory backend** – Searches provided arrays of paths/data/size for matches, supporting gzipped priorities identical to the filesystem flow.

Within the handler, developers must call exactly one of `sendStatic()`, `sendFile()`, or `redirect()` to complete the response.

### Example Usage

```cpp
server.serveStatic("/view", LittleFS, "/tmpl",
  [&](const StaticInfo& info, Request& req, Response& res){
      res.setTemplateHandler(...);
      res.setHeadInjection("<script src='/app.js'></script>");
      res.sendStatic();
  });
```

```cpp
server.serveStatic("/static", g_paths, g_data, g_sizes, g_fileCount,
  [&](const StaticInfo& info, Request& req, Response& res){
      if (!info.isGzipped && info.logicalPath.endsWith(".html")) {
          res.setHeadInjection("<script src='/static/app.js'></script>");
      }
      res.sendStatic();
  });
```

For SPA fallbacks, check `info.exists` and fall back to `sendFile()` with your index document when needed.

### Debug Levels

Select the Arduino board's **Core Debug Level** (or pass `--build-property build.code.debug=<level>` when using `arduino-cli`) to control logging. EspHttpServer relies on `ESP_LOGx` macros and the default `None` suppresses every log (even `ESP_LOGE`), so raise it during development if you want diagnostics:

- `None` – disables all logging
- `Error` – only critical failures (`ESP_LOGE`)
- `Info` – request lines and static route resolutions
- `Debug` – parameter dumps and detailed static file resolution

## Examples

- **BasicDynamic** – Minimal dynamic handler returning plain text.
- **ChunkedStream** – Streams JSON via chunked transfer.
- **TemplateHead** – Demonstrates template handlers plus head injection.
- **StaticFS** – Serves files uploaded from `examples/StaticFS/data` (use Arduino IDE/CLI upload or VS Code extension).
- **EmbeddedAssetsSimple** – Demonstrates converting `examples/EmbeddedAssetsSimple/assets_www` into `assets_www_embed.h` and serving flash-resident files over `/embed`.
- **PathParams** – Shows multi-parameter routes (`:id`) and wildcard captures via `req.pathParam()`.

## VS Code Workflow

The [Arduino CLI Wrapper](https://marketplace.visualstudio.com/items?itemName=tanakamasayuki.vscode-arduino-cli-wrapper) extension can:

- Upload `data/` folders (e.g., `examples/StaticFS/data`) to LittleFS/SPIFFS targets directly from VS Code.
- Convert asset folders (e.g., `examples/assetsTest/assets_www`) into header files such as `assets_www_embed.h`, honoring `.assetsconfig` for minification and gzip output.

Re-run the extension each time you edit assets so that the generated headers stay in sync with the source files.

## Next Steps

1. Implement the `Request`/`Response` classes and wire them to `esp_http_server`.
2. Build filesystem adapters (LittleFS, SPIFFS, SD) and memory bundles for static assets.
3. Provide utilities for gzip generation and MIME type resolution.
