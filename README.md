# ESP32 HTTP Server Library

[Japanese README](README.ja.md)

This repository documents a lightweight HTTP server stack for ESP32/Arduino built on top of `esp_http_server`. The goal is to ship a coherent set of behaviors—dynamic handlers, template & head injections, filesystem/in-memory static serving with gzip, and consistent error hooks—so that sketches can follow the same rules. The full specification lives in [SPEC.md](SPEC.md).

Start with the [example workflow](docs/GUIDE_EXAMPLES.md) and [individual example READMEs](examples/README.md) for setup, API sequence, verification and adaptation notes.

## Highlights

- **Response Stack** – `send()`/`sendText()`/`sendStatic()`/`sendFile()` plus chunked helpers, redirects, and a `sendError()` path that feeds a global `ErrorRenderer`.
- **Templates & Head Injection** – Streamed HTML renderer handles `{{key}}` (escaped) / `{{{key}}}` (raw). Head injection drops CSP/script/meta snippets right after `<head>` so you can toggle analytics or policy tags without editing every file. Both features automatically disable themselves for gzipped payloads, so precompressed assets stay untouched.
- **Static Lifecycle** – `serveStatic()` resolves filesystem or memory assets, prefers `.gz` siblings, auto-detects `index.html|htm`, and even auto-sends (or 404s) if a handler forgets to respond.
- **Routing & Fallbacks** – `on()` supports literal/param/wildcard scoring, `onNotFound()` acts as a catch-all, and unhandled requests automatically return 500/404 instead of stalling.
- **Logging Discipline** – All response logs share the `[RESP][tag] <code> ...` format and respect the board’s Core Debug Level so you can dial verbosity from “silent” to “deep internals”.

## Specification Overview

### Response API
```cpp
void send(int code, const char* type,
          const uint8_t* data, size_t len);
void sendText(int code, const char* type,
              const String& text);
void beginChunked(int code, const char* type);
void sendChunk(const char* text);
void sendStatic();
void sendFile(fs::FS& fs, const String& fsPath);
void sendError(int status);
void redirect(const char* location, int status = 302);
```
- HTML responses (including `send()` / `sendText()`) pass through the streaming template + head injection pipeline when `Content-Type` is `text/html` and the payload is not gzipped.
- `sendStatic()`/`sendFile()` stream data directly from FS or memory chunks; gzipped assets automatically set `Content-Encoding: gzip` and skip templating.
- `sendError()` sets the status, logs `[RESP][ERR] <code>`, and either invokes the global `ErrorRenderer` or emits a built-in short message (e.g., “Not Found”).
- `Response::setErrorRenderer()` lets sketches render custom HTML/JSON error pages without rewriting every handler.

### Static Serving Lifecycle
- `serveStatic("/www", LittleFS, "/wwwroot", handler)` normalizes the URI, joins it with `basePath`, prefers `.gz`, detects directories, and probes `index.html` then `index.htm`.
- The memory variant accepts parallel arrays (`paths`, `data`, `sizes`) and mirrors the same gzip + directory probing logic.
- Handlers still get the final say (e.g., SPA fallback). But if they **forget** to call `sendStatic()`/`sendFile()`/`sendError()`/`redirect()`, the library auto-falls back:
  - `StaticInfo.exists == true` → call `sendStatic()` automatically.
  - Otherwise → emit `sendError(404)`.

### Routing & Catch-alls
- `server.on("/user/:id", HTTP_GET, handler)` parses segments into literal/parameter/wildcard buckets. Routes are scored (`literal +3`, `param +2`, `wildcard +1`) and registration order breaks ties.
- If **no** dynamic route matches, `onNotFound()` runs. Leaving that handler without a response triggers an automatic 404.
- If a dynamic handler (or `onNotFound()`) returns without sending, the library calls `sendError(500)` (or 404 for the catch-all) so connections never hang.

### Logging & Debug Levels
- `[RESP]` logs are uniform: `send()` logs `[RESP] 200 text/html 512 bytes`, `sendStatic()` logs `[RESP][STATIC][FS] 200 /index.html (plain) origin=/wwwroot/index.html`, errors use `[RESP][ERR] 404`.
- Choose **Core Debug Level** (None/Error/Info/Debug) in the Arduino IDE or via `arduino-cli` build flags. `None` suppresses every log—even `ESP_LOGE`.

## Examples (see [`examples/`](examples/))

Every sketch uses `arduino_secrets.h` when available and otherwise falls back to the
`WIFI_SSID` / `WIFI_PASS` placeholders in the sketch.

| Example | Entry point | What it demonstrates |
| --- | --- | --- |
| [BasicDynamic](examples/BasicDynamic/) | `GET /` | The smallest dynamic route, returning plain text with `sendText()`. |
| [ChunkedStream](examples/ChunkedStream/) | `GET /stream` | A JSON response streamed with `beginChunked()`, `sendChunk()`, and `endChunked()`. |
| [CookieSimple](examples/CookieSimple/) | `GET /` | Reading, setting, and clearing a visit-counter cookie, with the count rendered into an HTML template. |
| [EmbeddedAssetsSimple](examples/EmbeddedAssetsSimple/) | `GET /embed/` | Serving a generated in-memory asset bundle with `serveStatic()` and redirecting missing paths to its index page. |
| [ErrorHandling](examples/ErrorHandling/) | `GET /ok`, `GET /fail`, or any missing path | A global `Response::setErrorRenderer()`, explicit `sendError()`, and an `onNotFound()` handler. |
| [FormParamsSimple](examples/FormParamsSimple/) | `GET /`, then `POST /submit` | Reading and iterating `application/x-www-form-urlencoded` fields with `formParam()` and `forEachFormParam()`. |
| [LoginPlainBind](examples/LoginPlainBind/) | `GET /`, then `POST /login` | An embedded PlainBind UI fed by head-injected JSON, form login/logout, and server-side session data stored with AssocTree. This sketch also depends on `AssocTree` and `EspHelper`. |
| [MultipartParamsSimple](examples/MultipartParamsSimple/) | `GET /`, then `POST /upload` | Streaming multipart fields and file content with `onMultipart()`. |
| [PathParams](examples/PathParams/) | `GET /users/...` or `GET /files/...` | Named `:parameters`, a trailing `*wildcard`, and lookup through `pathParam()`. |
| [QueryParamsSimple](examples/QueryParamsSimple/) | `GET /?name=esp32&lang=en` | Reading named query parameters and iterating all of them. |
| [SessionSimple](examples/SessionSimple/) | `GET /` | Creating, rotating, and clearing session IDs backed by a fixed 16-slot in-memory store. |
| [StaticFS](examples/StaticFS/) | `GET /www/` | Serving `data/wwwroot` from LittleFS at `/www`, with `index.html` lookup and an SPA fallback. Upload the sketch's `data/` directory before running it. |
| [TemplateHead](examples/TemplateHead/) | `GET /template` | Escaped/raw template placeholders and `setHeadInjection()` in the same HTML response. |

## Tooling Workflow
- The [Arduino CLI Wrapper](https://marketplace.visualstudio.com/items?itemName=tanakamasayuki.vscode-arduino-cli-wrapper) VS Code extension uploads `data/` folders to FS targets and converts asset directories into header bundles (`assets_www_embed.h`) with optional gzip/minify.
- Re-run the generator whenever assets change so the embedded headers stay in sync.
