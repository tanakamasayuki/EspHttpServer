#include "EspHttpServer.h"

#include <esp_log.h>
#include <cstring>
#include <algorithm>

namespace EspHttpServer
{

    namespace
    {
        const char *TAG = "EspHttpServer";

        struct MimeEntry
        {
            const char *extension;
            const char *type;
        };

        const MimeEntry kMimeTable[] = {
            {".avif", "image/avif"},
            {".css", "text/css"},
            {".csv", "text/csv"},
            {".gif", "image/gif"},
            {".htm", "text/html"},
            {".html", "text/html"},
            {".ico", "image/x-icon"},
            {".jpeg", "image/jpeg"},
            {".jpg", "image/jpeg"},
            {".js", "application/javascript"},
            {".json", "application/json"},
            {".mjs", "application/javascript"},
            {".mp3", "audio/mpeg"},
            {".mp4", "video/mp4"},
            {".png", "image/png"},
            {".svg", "image/svg+xml"},
            {".txt", "text/plain"},
            {".wasm", "application/wasm"},
            {".webp", "image/webp"},
            {".xml", "application/xml"},
            {".zip", "application/zip"},
        };

        class StringBuilderPrint : public Print
        {
        public:
            explicit StringBuilderPrint(String &target) : _target(target) {}

            size_t write(uint8_t c) override
            {
                _target += static_cast<char>(c);
                return 1;
            }

            size_t write(const uint8_t *buffer, size_t size) override
            {
                _target.reserve(_target.length() + size);
                for (size_t i = 0; i < size; ++i)
                {
                    _target += static_cast<char>(buffer[i]);
                }
                return size;
            }

        private:
            String &_target;
        };

        String toStatusString(int code)
        {
            char buffer[16];
            snprintf(buffer, sizeof(buffer), "%d", code);
            return String(buffer);
        }

        String determineMimeType(const String &path)
        {
            if (path.isEmpty())
            {
                return String("application/octet-stream");
            }
            String lower = path;
            lower.toLowerCase();
            for (const auto &entry : kMimeTable)
            {
                if (lower.endsWith(entry.extension))
                {
                    return String(entry.type);
                }
            }
            if (lower.endsWith(".gz"))
            {
                // remove .gz and retry for compressed assets
                String plain = lower.substring(0, lower.length() - 3);
                for (const auto &entry : kMimeTable)
                {
                    if (plain.endsWith(entry.extension))
                    {
                        return String(entry.type);
                    }
                }
            }
            return String("application/octet-stream");
        }

        String htmlEscape(const String &input)
        {
            String out;
            out.reserve(input.length());
            for (size_t i = 0; i < input.length(); ++i)
            {
                const char c = input.charAt(i);
                switch (c)
                {
                case '&':
                    out += F("&amp;");
                    break;
                case '<':
                    out += F("&lt;");
                    break;
                case '>':
                    out += F("&gt;");
                    break;
                case '"':
                    out += F("&quot;");
                    break;
                case '\'':
                    out += F("&#39;");
                    break;
                default:
                    out += c;
                    break;
                }
            }
            return out;
        }

        bool isHtmlMime(const String &mime)
        {
            return mime.equalsIgnoreCase("text/html");
        }

        String injectHeadSnippet(const String &html, const char *snippet)
        {
            if (!snippet || !snippet[0])
            {
                return html;
            }
            String lower = html;
            lower.toLowerCase();
            int headPos = lower.indexOf("<head");
            if (headPos < 0)
            {
                return html;
            }
            int closePos = lower.indexOf('>', headPos);
            if (closePos < 0)
            {
                return html;
            }
            const int insertPos = closePos + 1;
            String out;
            out.reserve(html.length() + strlen(snippet));
            out += html.substring(0, insertPos);
            out += snippet;
            out += html.substring(insertPos);
            return out;
        }

        String renderTemplate(const String &input, TemplateHandler handler)
        {
            if (!handler)
            {
                return input;
            }

            String out;
            out.reserve(input.length());

            size_t i = 0;
            while (i < static_cast<size_t>(input.length()))
            {
                if (input.charAt(i) == '{' && i + 1 < static_cast<size_t>(input.length()) && input.charAt(i + 1) == '{')
                {
                    const bool triple = (i + 2 < static_cast<size_t>(input.length()) && input.charAt(i + 2) == '{');
                    const size_t openLen = triple ? 3 : 2;
                    const String closeToken = triple ? String("}}}") : String("}}");
                    const int closeIdx = input.indexOf(closeToken, i + openLen);
                    if (closeIdx < 0)
                    {
                        out += input.substring(i);
                        break;
                    }
                    const String keyRaw = input.substring(i + openLen, closeIdx);
                    String key = keyRaw;
                    key.trim();
                    bool handled = false;
                    String replacement;
                    if (!key.isEmpty())
                    {
                        StringBuilderPrint printer(replacement);
                        handled = handler(key, printer);
                    }
                    if (handled)
                    {
                        out += triple ? replacement : htmlEscape(replacement);
                    }
                    else
                    {
                        out += input.substring(i, closeIdx + closeToken.length());
                    }
                    i = closeIdx + closeToken.length();
                }
                else
                {
                    out += input.charAt(i);
                    ++i;
                }
            }
            return out;
        }

        bool streamFileToClient(httpd_req_t *raw, fs::FS *fs, const String &path)
        {
            if (!fs)
            {
                return false;
            }
            File file = fs->open(path, "r");
            if (!file)
            {
                ESP_LOGE(TAG, "Failed to open %s", path.c_str());
                return false;
            }
            uint8_t buffer[1024];
            while (file.available())
            {
                const size_t readLen = file.read(buffer, sizeof(buffer));
                if (readLen == 0)
                {
                    break;
                }
                if (httpd_resp_send_chunk(raw, reinterpret_cast<const char *>(buffer), readLen) != ESP_OK)
                {
                    file.close();
                    return false;
                }
            }
            file.close();
            httpd_resp_send_chunk(raw, nullptr, 0);
            return true;
        }

        bool streamMemoryToClient(httpd_req_t *raw, const uint8_t *data, size_t size)
        {
            if (!raw)
            {
                return false;
            }
            if (!data && size > 0)
            {
                return false;
            }
            if (size == 0)
            {
                httpd_resp_send_chunk(raw, nullptr, 0);
                return true;
            }
            const size_t chunkSize = 1024;
            size_t offset = 0;
            while (offset < size)
            {
                const size_t len = std::min(chunkSize, size - offset);
                if (httpd_resp_send_chunk(raw, reinterpret_cast<const char *>(data + offset), len) != ESP_OK)
                {
                    return false;
                }
                offset += len;
            }
            httpd_resp_send_chunk(raw, nullptr, 0);
            return true;
        }

        String readFileToString(fs::FS *fs, const String &path)
        {
            if (!fs)
            {
                return String();
            }
            File file = fs->open(path, "r");
            if (!file)
            {
                return String();
            }
            String out;
            out.reserve(file.size());
            while (file.available())
            {
                out += static_cast<char>(file.read());
            }
            file.close();
            return out;
        }

        String readMemoryToString(const uint8_t *data, size_t size)
        {
            if (!data || size == 0)
            {
                return String();
            }
            String out;
            out.reserve(size);
            for (size_t i = 0; i < size; ++i)
            {
                out += static_cast<char>(data[i]);
            }
            return out;
        }

        String ensureLeadingSlash(const String &path)
        {
            if (path.startsWith("/"))
            {
                return path;
            }
            return "/" + path;
        }

        bool extractRelativePath(const String &uri, const String &prefix, String &out)
        {
            if (!uri.startsWith(prefix))
            {
                return false;
            }
            if (uri.length() > prefix.length())
            {
                const char boundary = uri.charAt(prefix.length());
                if (!prefix.endsWith("/") && boundary != '/')
                {
                    return false;
                }
            }
            String rel = uri.substring(prefix.length());
            if (rel.isEmpty())
            {
                rel = "/";
            }
            rel = ensureLeadingSlash(rel);
            out = rel;
            return true;
        }

        String joinFsPath(const String &base, const String &rel)
        {
            String result = base;
            if (result.isEmpty())
            {
                result = "/";
            }
            if (!result.endsWith("/"))
            {
                result += "/";
            }
            String trimmed = rel;
            if (trimmed.startsWith("/"))
            {
                trimmed.remove(0, 1);
            }
            result += trimmed;
            return result;
        }

    } // namespace

    // -------- Request --------

    String Request::uri() const
    {
        return _raw ? String(_raw->uri) : String();
    }

    String Request::method() const
    {
        if (!_raw)
            return {};
        switch (_raw->method)
        {
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

    Response::Response(httpd_req_t *raw) { attachRequest(raw); }

    void Response::attachRequest(httpd_req_t *raw)
    {
        _raw = raw;
        _chunked = false;
    }

    void Response::setTemplateHandler(TemplateHandler handler)
    {
        _templateHandler = std::move(handler);
    }

    void Response::clearTemplateHandler()
    {
        _templateHandler = nullptr;
    }

    void Response::setHeadInjection(const char *snippet)
    {
        _headInjectionPtr = snippet;
        _headInjectionIsRawPtr = true;
        _headInjection.clear();
    }

    void Response::setHeadInjection(const String &snippet)
    {
        _headInjection = snippet;
        _headInjectionPtr = _headInjection.c_str();
        _headInjectionIsRawPtr = false;
    }

    void Response::clearHeadInjection()
    {
        _headInjectionPtr = nullptr;
        _headInjection.clear();
        _headInjectionIsRawPtr = false;
    }

    void Response::send(int code, const char *type, const uint8_t *data, size_t len)
    {
        if (!_raw)
            return;
        httpd_resp_set_type(_raw, type);
        const String status = toStatusString(code);
        httpd_resp_set_status(_raw, status.c_str());
        httpd_resp_send(_raw, reinterpret_cast<const char *>(data), len);
    }

    void Response::send(int code, const char *type, const String &body)
    {
        send(code, type, reinterpret_cast<const uint8_t *>(body.c_str()), body.length());
    }

    void Response::sendText(int code, const char *type, const char *text)
    {
        send(code, type, reinterpret_cast<const uint8_t *>(text), strlen(text));
    }

    void Response::sendText(int code, const char *type, const String &text)
    {
        send(code, type, reinterpret_cast<const uint8_t *>(text.c_str()), text.length());
    }

    void Response::beginChunked(int code, const char *type)
    {
        if (!_raw)
            return;
        _chunked = true;
        httpd_resp_set_type(_raw, type);
        const String status = toStatusString(code);
        httpd_resp_set_status(_raw, status.c_str());
    }

    void Response::sendChunk(const uint8_t *data, size_t len)
    {
        if (!_raw || !_chunked)
            return;
        httpd_resp_send_chunk(_raw, reinterpret_cast<const char *>(data), len);
    }

    void Response::sendChunk(const char *text)
    {
        sendChunk(reinterpret_cast<const uint8_t *>(text), strlen(text));
    }

    void Response::sendChunk(const String &text)
    {
        sendChunk(reinterpret_cast<const uint8_t *>(text.c_str()), text.length());
    }

    void Response::endChunked()
    {
        if (!_raw || !_chunked)
            return;
        httpd_resp_send_chunk(_raw, nullptr, 0);
        _chunked = false;
    }

    void Response::sendStatic()
    {
        if (!_raw)
            return;

        if (_staticSource == StaticSourceType::None)
        {
            ESP_LOGE(TAG, "Static source missing");
            httpd_resp_send_err(_raw, HTTPD_500_INTERNAL_SERVER_ERROR, "Static source missing");
            return;
        }

        if (!_staticInfo.exists)
        {
            httpd_resp_send_err(_raw, HTTPD_404_NOT_FOUND, "Not Found");
            return;
        }

        String logicalPath = _staticInfo.logicalPath;
        if (logicalPath.isEmpty())
        {
            logicalPath = _staticInfo.relPath;
        }
        const String mime = determineMimeType(logicalPath);
        httpd_resp_set_type(_raw, mime.c_str());
        httpd_resp_set_status(_raw, HTTPD_200);

        const bool htmlEligible = !_staticInfo.isGzipped && isHtmlMime(mime);
        if (_staticInfo.isGzipped)
        {
            httpd_resp_set_hdr(_raw, "Content-Encoding", "gzip");
        }

        if (!htmlEligible || (!_templateHandler && (!_headInjectionPtr || !_headInjectionPtr[0])))
        {
            bool ok = false;
            if (_staticSource == StaticSourceType::FileSystem)
            {
                ok = streamFileToClient(_raw, _staticFs, _staticInfo.fsPath);
            }
            else
            {
                ok = streamMemoryToClient(_raw, _memData, _memSize);
            }
            if (!ok)
            {
                httpd_resp_send_500(_raw);
            }
            return;
        }

        String body;
        if (_staticSource == StaticSourceType::FileSystem)
        {
            body = readFileToString(_staticFs, _staticInfo.fsPath);
        }
        else
        {
            body = readMemoryToString(_memData, _memSize);
        }

        if (body.isEmpty())
        {
            httpd_resp_send_500(_raw);
            return;
        }

        body = renderTemplate(body, _templateHandler);
        body = injectHeadSnippet(body, _headInjectionPtr);
        httpd_resp_send(_raw, body.c_str(), body.length());
    }

    void Response::sendFile(fs::FS &fs, const String &fsPath)
    {
        StaticInfo info;
        info.uri = fsPath;
        info.relPath = fsPath;
        info.fsPath = fsPath;
        info.exists = fs.exists(fsPath);
        info.isDir = false;
        info.isGzipped = fsPath.endsWith(".gz");
        info.logicalPath = info.isGzipped ? fsPath.substring(0, fsPath.length() - 3) : fsPath;
        setStaticFileSystem(&fs);
        setStaticInfo(info);
        sendStatic();
    }

    void Response::redirect(const char *location, int status)
    {
        if (!_raw)
            return;
        const String statusStr = toStatusString(status);
        httpd_resp_set_status(_raw, statusStr.c_str());
        httpd_resp_set_hdr(_raw, "Location", location);
        httpd_resp_send(_raw, nullptr, 0);
    }

    void Response::setStaticInfo(const StaticInfo &info)
    {
        _staticInfo = info;
    }

    void Response::setStaticFileSystem(fs::FS *fs)
    {
        _staticSource = fs ? StaticSourceType::FileSystem : StaticSourceType::None;
        _staticFs = fs;
        _memData = nullptr;
        _memSize = 0;
    }

    void Response::setStaticMemorySource(const uint8_t *data, size_t size)
    {
        if (data || size == 0)
        {
            _staticSource = StaticSourceType::Memory;
            _memData = data;
            _memSize = size;
        }
        else
        {
            clearStaticSource();
        }
        _staticFs = nullptr;
    }

    void Response::clearStaticSource()
    {
        _staticSource = StaticSourceType::None;
        _staticFs = nullptr;
        _memData = nullptr;
        _memSize = 0;
    }

    // -------- Server --------

    Server::Server() = default;
    Server::~Server() { end(); }

    bool Server::begin(const httpd_config_t &cfg)
    {
        if (_handle)
            return true;
        esp_err_t err = httpd_start(&_handle, &cfg);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
            return false;
        }
        for (auto &entry : _handlers)
        {
            registerHandler(entry.get());
        }
        return true;
    }

    void Server::end()
    {
        if (_handle)
        {
            httpd_stop(_handle);
            _handle = nullptr;
        }
    }

    void Server::on(const String &uri, httpd_method_t method, RouteHandler handler)
    {
        if (!handler)
        {
            return;
        }

        auto entry = std::make_unique<HandlerEntry>();
        entry->type = HandlerType::Dynamic;
        entry->routeHandler = std::move(handler);
        entry->uriPrefix = uri;
        entry->uriPattern = uri;
        entry->uriDef.uri = entry->uriPattern.c_str();
        entry->uriDef.method = method;
        entry->uriDef.handler = &Server::handleHttpRequest;
        entry->uriDef.user_ctx = entry.get();
        entry->uriDef.uri_match_fn = nullptr;
        entry->owner = this;

        HandlerEntry *entryPtr = entry.get();
        _handlers.push_back(std::move(entry));
        if (_handle)
        {
            registerHandler(entryPtr);
        }
    }

    void Server::serveStatic(const String &uriPrefix,
                             fs::FS &fs,
                             const String &basePath,
                             StaticHandler handler)
    {
        if (!handler)
        {
            return;
        }
        auto entry = std::make_unique<HandlerEntry>();
        entry->type = HandlerType::StaticFS;
        entry->staticHandler = std::move(handler);
        entry->uriPrefix = uriPrefix;
        entry->basePath = basePath;
        entry->fs = &fs;
        entry->uriPattern = uriPrefix;
        if (!entry->uriPattern.endsWith("*"))
        {
            entry->uriPattern += "*";
        }
        entry->uriDef.uri = entry->uriPattern.c_str();
        entry->uriDef.method = HTTP_GET;
        entry->uriDef.handler = &Server::handleHttpRequest;
        entry->uriDef.user_ctx = entry.get();
        entry->uriDef.uri_match_fn = httpd_uri_match_wildcard;
        entry->owner = this;

        HandlerEntry *entryPtr = entry.get();
        _handlers.push_back(std::move(entry));
        if (_handle)
        {
            registerHandler(entryPtr);
        }
    }

    void Server::serveStatic(const String &uriPrefix,
                             const char *const *paths,
                             const uint8_t *const *data,
                             const size_t *sizes,
                             size_t fileCount,
                             StaticHandler handler)
    {
        if (!handler || !paths || !data || !sizes)
        {
            return;
        }

        auto entry = std::make_unique<HandlerEntry>();
        entry->type = HandlerType::StaticMem;
        entry->staticHandler = std::move(handler);
        entry->uriPrefix = uriPrefix;
        entry->uriPattern = uriPrefix;
        if (!entry->uriPattern.endsWith("*"))
        {
            entry->uriPattern += "*";
        }
        entry->uriDef.uri = entry->uriPattern.c_str();
        entry->uriDef.method = HTTP_GET;
        entry->uriDef.handler = &Server::handleHttpRequest;
        entry->uriDef.user_ctx = entry.get();
        entry->uriDef.uri_match_fn = httpd_uri_match_wildcard;
        entry->memPaths = paths;
        entry->memData = data;
        entry->memSizes = sizes;
        entry->memCount = fileCount;
        entry->owner = this;

        HandlerEntry *entryPtr = entry.get();
        _handlers.push_back(std::move(entry));
        if (_handle)
        {
            registerHandler(entryPtr);
        }
    }

    esp_err_t Server::handleHttpRequest(httpd_req_t *req)
    {
        auto *entry = static_cast<HandlerEntry *>(req->user_ctx);
        if (!entry)
        {
            return ESP_FAIL;
        }

        Request request(req);
        Response response(req);

        switch (entry->type)
        {
        case HandlerType::Dynamic:
            if (entry->routeHandler)
            {
                entry->routeHandler(request, response);
            }
            break;
        case HandlerType::StaticFS:
            if (entry->owner)
            {
                entry->owner->setupStaticInfoFromFS(entry, request, response);
            }
            break;
        case HandlerType::StaticMem:
            if (entry->owner)
            {
                entry->owner->setupStaticInfoFromMemory(entry, request, response);
            }
            break;
        }

        return ESP_OK;
    }

    bool Server::registerHandler(HandlerEntry *entry)
    {
        if (!_handle || !entry)
        {
            return false;
        }
        entry->uriDef.user_ctx = entry;
        const esp_err_t err = httpd_register_uri_handler(_handle, &entry->uriDef);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to register %s: %s", entry->uriPattern.c_str(), esp_err_to_name(err));
            return false;
        }
        entry->registered = true;
        return true;
    }

    void Server::setupStaticInfoFromFS(HandlerEntry *entry, Request &req, Response &res)
    {
        if (!entry || !entry->fs)
        {
            httpd_resp_send_err(req.raw(), HTTPD_500_INTERNAL_SERVER_ERROR, "FS missing");
            return;
        }

        String uri = req.uri();
        String rel;
        if (!extractRelativePath(uri, entry->uriPrefix, rel))
        {
            httpd_resp_send_err(req.raw(), HTTPD_404_NOT_FOUND, "Not Found");
            return;
        }

        StaticInfo info;
        info.uri = uri;
        info.relPath = rel;
        info.logicalPath = rel;

        const bool requestGz = rel.endsWith(".gz");
        const String baseFsPath = joinFsPath(entry->basePath, rel);
        const String gzCandidate = baseFsPath + ".gz";

        bool exists = false;
        bool isDir = false;
        bool useGz = false;

        const bool hasGz = !requestGz && entry->fs->exists(gzCandidate);
        const bool hasPlain = entry->fs->exists(baseFsPath);

        if (hasGz)
        {
            info.fsPath = gzCandidate;
            exists = true;
            useGz = true;
        }
        else if (hasPlain)
        {
            info.fsPath = baseFsPath;
            exists = true;
            File test = entry->fs->open(baseFsPath, "r");
            if (test)
            {
                isDir = test.isDirectory();
                test.close();
            }
        }
        else
        {
            info.fsPath = baseFsPath;
        }

        info.exists = exists;
        info.isDir = isDir;
        info.isGzipped = useGz || requestGz;
        if (info.isGzipped && info.logicalPath.endsWith(".gz"))
        {
            info.logicalPath = info.logicalPath.substring(0, info.logicalPath.length() - 3);
        }

        res.setStaticFileSystem(entry->fs);
        res.setStaticInfo(info);

        if (entry->staticHandler)
        {
            entry->staticHandler(info, req, res);
        }
    }

    void Server::setupStaticInfoFromMemory(HandlerEntry *entry, Request &req, Response &res)
    {
        String uri = req.uri();
        String rel;
        if (!extractRelativePath(uri, entry->uriPrefix, rel))
        {
            httpd_resp_send_err(req.raw(), HTTPD_404_NOT_FOUND, "Not Found");
            return;
        }

        StaticInfo info;
        info.uri = uri;
        info.relPath = rel;
        info.logicalPath = rel;

        const bool requestGz = rel.endsWith(".gz");
        int exactIndex = -1;
        int gzIndex = -1;
        const String gzRel = rel + ".gz";
        for (size_t i = 0; i < entry->memCount; ++i)
        {
            const char *candidate = entry->memPaths[i];
            if (!candidate)
            {
                continue;
            }
            String candidateStr(candidate);
            if (rel == candidateStr)
            {
                exactIndex = static_cast<int>(i);
            }
            else if (!requestGz && candidateStr == gzRel)
            {
                gzIndex = static_cast<int>(i);
            }
        }

        int chosenIndex = -1;
        bool gz = false;
        if (!requestGz && gzIndex >= 0)
        {
            chosenIndex = gzIndex;
            gz = true;
        }
        else if (exactIndex >= 0)
        {
            chosenIndex = exactIndex;
            gz = requestGz;
        }

        if (chosenIndex >= 0)
        {
            info.exists = true;
            info.fsPath = entry->memPaths[chosenIndex];
            info.isGzipped = gz;
            if (info.isGzipped && info.logicalPath.endsWith(".gz"))
            {
                info.logicalPath = info.logicalPath.substring(0, info.logicalPath.length() - 3);
            }
            const uint8_t *dataPtr = entry->memData[chosenIndex];
            const size_t dataSize = entry->memSizes[chosenIndex];
            res.setStaticMemorySource(dataPtr, dataSize);
        }
        else
        {
            info.exists = false;
            info.isGzipped = false;
            res.clearStaticSource();
        }

        if (entry->staticHandler)
        {
            entry->staticHandler(info, req, res);
        }
    }

} // namespace EspHttpServer
