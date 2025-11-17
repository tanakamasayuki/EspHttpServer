#include "EspHttpServer.h"

#include <esp_log.h>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <lwip/sockets.h>
#include <lwip/ip4_addr.h>

#ifdef CORE_DEBUG_LEVEL
#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL CORE_DEBUG_LEVEL
#endif

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

    class StaticInputStream
    {
    public:
        StaticInputStream(fs::FS *fs, const String &path)
            : _fs(fs), _useFs(true)
        {
            if (_fs)
            {
                _file = _fs->open(path, "r");
            }
        }

        StaticInputStream(const uint8_t *data, size_t size)
            : _fs(nullptr), _useFs(false), _data(data), _size(size)
        {
        }

        ~StaticInputStream()
        {
            if (_file)
            {
                _file.close();
            }
        }

        bool valid() const
        {
            if (_useFs)
            {
                return static_cast<bool>(_file);
            }
            return (_data != nullptr) || (_size == 0);
        }

        bool readChar(char &out)
        {
            if (_useFs)
            {
                if (!_file)
                {
                    return false;
                }
                if (_bufPos >= _bufLen)
                {
                    _bufLen = _file.read(_buffer, sizeof(_buffer));
                    _bufPos = 0;
                    if (_bufLen == 0)
                    {
                        return false;
                    }
                }
                out = static_cast<char>(_buffer[_bufPos++]);
                return true;
            }

            if (_pos >= _size)
            {
                return false;
            }
            if (_data)
            {
                out = static_cast<char>(_data[_pos]);
            }
            else
            {
                out = 0;
            }
            ++_pos;
            return true;
        }

    private:
        fs::FS *_fs = nullptr;
        bool _useFs = false;
        File _file;
        const uint8_t *_data = nullptr;
        size_t _size = 0;
        size_t _pos = 0;
        uint8_t _buffer[256];
        size_t _bufLen = 0;
        size_t _bufPos = 0;
    };

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

    String Request::pathParam(const String &key) const
    {
        for (const auto &entry : _pathParams)
        {
            if (entry.first == key)
            {
                return entry.second;
            }
        }
        return String();
    }

    bool Request::hasPathParam(const String &key) const
    {
        for (const auto &entry : _pathParams)
        {
            if (entry.first == key)
            {
                return true;
            }
        }
        return false;
    }

    void Request::setPathInfo(const String &path, const std::vector<std::pair<String, String>> &params)
    {
        _normalizedPath = path;
        _pathParams = params;
    }

    void Request::clearPathInfo()
    {
        _normalizedPath = "/";
        _pathParams.clear();
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
        httpd_resp_set_status(_raw, statusString(code));
        httpd_resp_send(_raw, reinterpret_cast<const char *>(data), len);
        ESP_LOGI(TAG, "[RESP] %d %s %zu bytes", code, type ? type : "-", len);
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
        httpd_resp_set_status(_raw, statusString(code));
        ESP_LOGI(TAG, "[RESP] %d %s (chunked)", code, type ? type : "-");
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
        ESP_LOGI(TAG, "[RESP] chunked end");
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
            String missingPath = _staticInfo.logicalPath;
            if (missingPath.isEmpty())
            {
                missingPath = _staticInfo.relPath;
            }
            ESP_LOGI(TAG, "[RESP] 404 static %s", missingPath.c_str());
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

        const bool needsProcessing = htmlEligible && (_templateHandler || (_headInjectionPtr && _headInjectionPtr[0]));
        if (!needsProcessing)
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
                ESP_LOGE(TAG, "[RESP] 500 static stream failed (%s)", logicalPath.c_str());
                httpd_resp_send_500(_raw);
            }
            return;
        }

        bool ok = false;
        if (_staticSource == StaticSourceType::FileSystem)
        {
            StaticInputStream stream(_staticFs, _staticInfo.fsPath);
            ok = streamHtmlFromSource(stream);
        }
        else
        {
            StaticInputStream stream(_memData, _memSize);
            ok = streamHtmlFromSource(stream);
        }
        if (!ok)
        {
            ESP_LOGE(TAG, "[RESP] 500 static html stream failed (%s)", logicalPath.c_str());
            httpd_resp_send_500(_raw);
        }
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
        const char *statusStr = statusString(status);
        httpd_resp_set_status(_raw, statusStr);
        httpd_resp_set_hdr(_raw, "Location", location);
        httpd_resp_send(_raw, nullptr, 0);
        ESP_LOGI(TAG, "[RESP] %s redirect -> %s", statusStr, location);
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

    const char *Response::statusString(int code)
    {
        snprintf(_statusBuffer, sizeof(_statusBuffer), "%d", code);
        return _statusBuffer;
    }

    bool Response::streamHtmlFromSource(StaticInputStream &stream)
    {
        if (!_raw || !stream.valid())
        {
            return false;
        }

        constexpr size_t kChunkLimit = 512;
        String chunk;
        chunk.reserve(kChunkLimit);

        const bool templateActive = static_cast<bool>(_templateHandler);
        const char *headSnippet = (_headInjectionPtr && _headInjectionPtr[0]) ? _headInjectionPtr : nullptr;
        bool snippetInserted = (headSnippet == nullptr);
        constexpr char kHeadToken[] = "<head";
        constexpr int kHeadTokenLen = sizeof(kHeadToken) - 1;
        int headMatchIdx = 0;
        bool awaitingHeadBoundary = false;
        bool waitingHeadClose = false;

        auto flushChunk = [&]() -> bool
        {
            if (chunk.isEmpty())
            {
                return true;
            }
            if (httpd_resp_send_chunk(_raw, chunk.c_str(), chunk.length()) != ESP_OK)
            {
                return false;
            }
            chunk.clear();
            return true;
        };

        auto appendRawChar = [&](char c) -> bool
        {
            chunk += c;
            if (chunk.length() >= kChunkLimit)
            {
                return flushChunk();
            }
            return true;
        };

        auto appendRawString = [&](const char *text) -> bool
        {
            if (!text)
            {
                return true;
            }
            while (*text)
            {
                if (!appendRawChar(*text++))
                {
                    return false;
                }
            }
            return true;
        };

        auto emitChar = [&](char c) -> bool
        {
            if (!snippetInserted)
            {
                const char lower = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                if (!awaitingHeadBoundary && !waitingHeadClose)
                {
                    if (lower == kHeadToken[headMatchIdx])
                    {
                        headMatchIdx++;
                        if (headMatchIdx == kHeadTokenLen)
                        {
                            awaitingHeadBoundary = true;
                            headMatchIdx = 0;
                        }
                    }
                    else
                    {
                        if (lower == kHeadToken[0])
                        {
                            headMatchIdx = 1;
                        }
                        else
                        {
                            headMatchIdx = 0;
                        }
                    }
                }
                else if (awaitingHeadBoundary)
                {
                    if (c == '>' || c == '/' || isspace(static_cast<unsigned char>(c)))
                    {
                        awaitingHeadBoundary = false;
                        if (c == '>')
                        {
                            if (!appendRawChar(c))
                            {
                                return false;
                            }
                            if (headSnippet && headSnippet[0])
                            {
                                if (!appendRawString(headSnippet))
                                {
                                    return false;
                                }
                            }
                            snippetInserted = true;
                            return true;
                        }
                        waitingHeadClose = true;
                    }
                    else
                    {
                        awaitingHeadBoundary = false;
                    }
                }
                else if (waitingHeadClose && c == '>')
                {
                    waitingHeadClose = false;
                    if (!appendRawChar(c))
                    {
                        return false;
                    }
                    if (headSnippet && headSnippet[0])
                    {
                        if (!appendRawString(headSnippet))
                        {
                            return false;
                        }
                    }
                    snippetInserted = true;
                    return true;
                }
            }
            return appendRawChar(c);
        };

        auto emitString = [&](const String &text) -> bool
        {
            for (size_t i = 0; i < text.length(); ++i)
            {
                if (!emitChar(text.charAt(i)))
                {
                    return false;
                }
            }
            return true;
        };

        auto emitRepeat = [&](char c, int count) -> bool
        {
            for (int i = 0; i < count; ++i)
            {
                if (!emitChar(c))
                {
                    return false;
                }
            }
            return true;
        };

        enum class TemplateState
        {
            Normal,
            OpenBrace,
            Placeholder
        };

        TemplateState state = TemplateState::Normal;
        int braceCount = 0;
        bool waitingThird = false;
        bool triple = false;
        int closingCount = 0;
        String placeholderRaw;

        char ch;
        while (stream.readChar(ch))
        {
            bool reprocess = true;
            while (reprocess)
            {
                reprocess = false;
                switch (state)
                {
                case TemplateState::Normal:
                    if (templateActive && ch == '{')
                    {
                        state = TemplateState::OpenBrace;
                        braceCount = 1;
                    }
                    else
                    {
                        if (!emitChar(ch))
                        {
                            return false;
                        }
                    }
                    break;
                case TemplateState::OpenBrace:
                    if (templateActive && ch == '{')
                    {
                        braceCount++;
                        if (braceCount == 2)
                        {
                            state = TemplateState::Placeholder;
                            placeholderRaw.clear();
                            waitingThird = true;
                            triple = false;
                            closingCount = 0;
                        }
                    }
                    else
                    {
                        if (!emitRepeat('{', braceCount))
                        {
                            return false;
                        }
                        braceCount = 0;
                        state = TemplateState::Normal;
                        reprocess = true;
                    }
                    break;
                case TemplateState::Placeholder:
                    if (waitingThird)
                    {
                        if (ch == '{')
                        {
                            triple = true;
                            waitingThird = false;
                            continue;
                        }
                        waitingThird = false;
                        reprocess = true;
                        continue;
                    }
                    if (ch == '}')
                    {
                        closingCount++;
                        const int needed = triple ? 3 : 2;
                        if (closingCount == needed)
                        {
                            String key = placeholderRaw;
                            key.trim();
                            bool handled = false;
                            String replacement;
                            if (_templateHandler && !key.isEmpty())
                            {
                                StringBuilderPrint printer(replacement);
                                handled = _templateHandler(key, printer);
                            }
                            if (handled)
                            {
                                if (!triple)
                                {
                                    replacement = htmlEscape(replacement);
                                }
                                if (!emitString(replacement))
                                {
                                    return false;
                                }
                            }
                            else
                            {
                                if (!emitRepeat('{', needed))
                                {
                                    return false;
                                }
                                if (!emitString(placeholderRaw))
                                {
                                    return false;
                                }
                                if (!emitRepeat('}', needed))
                                {
                                    return false;
                                }
                            }
                            placeholderRaw.clear();
                            closingCount = 0;
                            triple = false;
                            state = TemplateState::Normal;
                            braceCount = 0;
                        }
                        continue;
                    }
                    if (closingCount > 0)
                    {
                        for (int i = 0; i < closingCount; ++i)
                        {
                            placeholderRaw += '}';
                        }
                        closingCount = 0;
                    }
                    placeholderRaw += ch;
                    break;
                }
            }
        }

        if (state == TemplateState::OpenBrace && braceCount > 0)
        {
            if (!emitRepeat('{', braceCount))
            {
                return false;
            }
        }
        else if (state == TemplateState::Placeholder)
        {
            const int needed = triple ? 3 : 2;
            if (!emitRepeat('{', needed))
            {
                return false;
            }
            if (!emitString(placeholderRaw))
            {
                return false;
            }
            if (closingCount > 0)
            {
                if (!emitRepeat('}', closingCount))
                {
                    return false;
                }
            }
        }

        if (!flushChunk())
        {
            return false;
        }
        return httpd_resp_send_chunk(_raw, nullptr, 0) == ESP_OK;
    }

    // -------- Server --------

    Server::Server() = default;
    Server::~Server() { end(); }

    bool Server::begin(const httpd_config_t &cfg)
    {
        if (_handle)
            return true;
        httpd_config_t localCfg = cfg;
        localCfg.uri_match_fn = httpd_uri_match_wildcard;
        esp_err_t err = httpd_start(&_handle, &localCfg);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
            return false;
        }
        for (auto &hook : _methodHooks)
        {
            registerMethodHook(hook.get());
        }
        return true;
    }

#if LOG_LOCAL_LEVEL >= ESP_LOG_INFO
        String normalizeBasePathForLog(const String &input)
        {
            String normalized = input;
            if (normalized.isEmpty())
            {
                normalized = "/";
            }
            while (normalized.length() > 1 && normalized.endsWith("/"))
            {
                normalized.remove(normalized.length() - 1);
            }
            if (!normalized.startsWith("/"))
            {
                normalized = "/" + normalized;
            }
            return normalized;
        }

        String relativePathForLog(const String &normalizedBase, const String &fullPath)
        {
            if (fullPath.isEmpty())
            {
                return String("/");
            }
            String relative = fullPath;
            if (normalizedBase != "/")
            {
                if (relative.startsWith(normalizedBase))
                {
                    relative = relative.substring(normalizedBase.length());
                }
                else
                {
                    String baseNoSlash = normalizedBase.substring(1);
                    if (relative.startsWith(baseNoSlash))
                    {
                        relative = relative.substring(baseNoSlash.length());
                    }
                }
            }
            if (relative.startsWith("/"))
            {
                relative.remove(0, 1);
            }
            if (relative.isEmpty())
            {
                return String("/");
            }
            return relative;
        }

        String makeLogIndent(int depth)
        {
            String indent;
            if (depth <= 0)
            {
                return indent;
            }
            indent.reserve(depth * 2);
            for (int i = 0; i < depth; ++i)
            {
                indent += "  ";
            }
            return indent;
        }

        String buildFullPath(const String &base, const String &entryName)
        {
            if (entryName.isEmpty())
            {
                return base;
            }
            if (entryName.startsWith("/"))
            {
                return entryName;
            }
            if (base == "/")
            {
                return String("/") + entryName;
            }
            String combined = base;
            if (!combined.endsWith("/"))
            {
                combined += "/";
            }
            combined += entryName;
            return combined;
        }

        void logFsDirectory(fs::FS &fs, const String &normalizedBase, const String &fsPath, int depth)
        {
            File dir = fs.open(fsPath);
            if (!dir || !dir.isDirectory())
            {
                if (dir)
                {
                    dir.close();
                }
                return;
            }

            while (true)
            {
                File entry = dir.openNextFile();
                if (!entry)
                {
                    break;
                }
                const char *rawName = entry.name();
                String entryName = rawName ? String(rawName) : String();
                String fullPath = buildFullPath(fsPath, entryName);
                if (fullPath.isEmpty())
                {
                    fullPath = fsPath;
                }
                String relative = relativePathForLog(normalizedBase, fullPath);
                String indent = makeLogIndent(depth);
                if (entry.isDirectory())
                {
                    ESP_LOGI(TAG, "  %s%s/ [%s]", indent.c_str(), relative.c_str(), fullPath.c_str());
                    entry.close();
                    logFsDirectory(fs, normalizedBase, fullPath, depth + 1);
                }
                else
                {
                    size_t size = entry.size();
                    bool gz = fullPath.endsWith(".gz");
                    ESP_LOGI(TAG, "  %s%s (%u bytes)%s [%s]",
                             indent.c_str(),
                             relative.c_str(),
                             static_cast<unsigned>(size),
                             gz ? " gz" : "",
                             fullPath.c_str());
                    entry.close();
                }
            }

            dir.close();
        }

        void logFileSystemListing(fs::FS &fs, const String &basePath)
        {
            String openPath = basePath;
            if (openPath.isEmpty())
            {
                openPath = "/";
            }
            File root = fs.open(openPath);
            if (!root && !openPath.startsWith("/"))
            {
                String alt = "/" + openPath;
                root = fs.open(alt);
                if (root)
                {
                    openPath = alt;
                }
            }
            if (!root)
            {
                ESP_LOGW(TAG, "[SERVE][FS] unable to list %s", basePath.c_str());
                return;
            }

            String normalizedBase = normalizeBasePathForLog(openPath);
            if (!root.isDirectory())
            {
                size_t size = root.size();
                bool gz = normalizedBase.endsWith(".gz");
                ESP_LOGI(TAG, "[SERVE][FS] %s (%u bytes)%s [%s]",
                         normalizedBase.c_str(),
                         static_cast<unsigned>(size),
                         gz ? " gz" : "",
                         normalizedBase.c_str());
                root.close();
                return;
            }

            ESP_LOGI(TAG, "[SERVE][FS] listing %s", normalizedBase.c_str());
            root.close();
            logFsDirectory(fs, normalizedBase, normalizedBase, 0);
        }
#endif

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

        std::vector<RouteSegment> segments;
        int score = 0;
        if (!parseRoutePattern(uri, segments, score))
        {
            ESP_LOGE(TAG, "Invalid route pattern: %s", uri.c_str());
            return;
        }

        if (!ensureMethodHook(method))
        {
            ESP_LOGE(TAG, "Failed to register method hook");
            return;
        }

        DynamicRoute route;
        route.method = method;
        route.pattern = uri;
        route.segments = std::move(segments);
        route.score = score;
        route.handler = std::move(handler);
        _dynamicRoutes.push_back(std::move(route));
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
        String prefix = uriPrefix;
        if (prefix.isEmpty())
        {
            prefix = "/";
        }
        if (!prefix.startsWith("/"))
        {
            prefix = "/" + prefix;
        }
        while (prefix.length() > 1 && prefix.endsWith("/"))
        {
            prefix.remove(prefix.length() - 1);
        }
        entry->uriPrefix = prefix;
        entry->basePath = basePath;
        entry->fs = &fs;
        entry->owner = this;

#if LOG_LOCAL_LEVEL >= ESP_LOG_INFO
        ESP_LOGI(TAG, "[SERVE][FS] %s -> %s", entry->uriPrefix.c_str(), entry->basePath.c_str());
        logFileSystemListing(fs, basePath);
#endif

        _handlers.push_back(std::move(entry));
        ensureMethodHook(HTTP_GET);
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
        String prefix = uriPrefix;
        if (prefix.isEmpty())
        {
            prefix = "/";
        }
        if (!prefix.startsWith("/"))
        {
            prefix = "/" + prefix;
        }
        while (prefix.length() > 1 && prefix.endsWith("/"))
        {
            prefix.remove(prefix.length() - 1);
        }
        entry->uriPrefix = prefix;
        entry->memPaths = paths;
        entry->memData = data;
        entry->memSizes = sizes;
        entry->memCount = fileCount;
        entry->owner = this;

#if LOG_LOCAL_LEVEL >= ESP_LOG_INFO
        ESP_LOGI(TAG, "[SERVE][MEM] %s count=%u", entry->uriPrefix.c_str(), static_cast<unsigned>(fileCount));
        for (size_t i = 0; i < fileCount; ++i)
        {
            const char *name = paths[i] ? paths[i] : "(null)";
            size_t size = sizes[i];
            bool gz = false;
            if (name)
            {
                size_t len = strlen(name);
                gz = (len >= 3 && name[len - 3] == '.' && name[len - 2] == 'g' && name[len - 1] == 'z');
            }
            ESP_LOGI(TAG, "  [%02u] %s (%u bytes)%s",
                     static_cast<unsigned>(i),
                     name,
                     static_cast<unsigned>(size),
                     gz ? " gz" : "");
        }
#endif

        _handlers.push_back(std::move(entry));
        ensureMethodHook(HTTP_GET);
    }

    esp_err_t Server::handleDynamicHttpRequest(httpd_req_t *req)
    {
        auto *server = static_cast<Server *>(req->user_ctx);
        if (!server)
        {
            return ESP_FAIL;
        }
        return server->dispatchDynamic(req);
    }

    void Server::setupStaticInfoFromFS(HandlerEntry *entry, Request &req, Response &res, const String &normalizedUri, const String &relPath)
    {
        if (!entry || !entry->fs)
        {
            ESP_LOGE(TAG, "[RESP] 500 static fs missing");
            httpd_resp_send_err(req.raw(), HTTPD_500_INTERNAL_SERVER_ERROR, "FS missing");
            return;
        }

        StaticInfo info;
        info.uri = normalizedUri;
        info.relPath = relPath;
        info.logicalPath = relPath;

        const bool requestGz = relPath.endsWith(".gz");
        String relBase = relPath;
        if (requestGz)
        {
            relBase = relPath.substring(0, relPath.length() - 3);
            if (relBase.isEmpty())
            {
                relBase = "/";
            }
        }
        const String plainFsPath = joinFsPath(entry->basePath, relBase);
        const String gzFsPath = plainFsPath + ".gz";

        bool exists = false;
        bool isDir = false;
        bool useGz = false;

        const bool hasGz = entry->fs->exists(gzFsPath);
        const bool hasPlain = entry->fs->exists(plainFsPath);

        if (requestGz)
        {
            if (hasGz)
            {
                info.fsPath = gzFsPath;
                exists = true;
                useGz = true;
            }
            else
            {
                info.fsPath = gzFsPath;
                exists = false;
                useGz = true;
            }
        }
        else if (hasGz)
        {
            info.fsPath = gzFsPath;
            exists = true;
            useGz = true;
        }
        else if (hasPlain)
        {
            info.fsPath = plainFsPath;
            exists = true;
            useGz = false;
            File test = entry->fs->open(plainFsPath, "r");
            if (test)
            {
                isDir = test.isDirectory();
                test.close();
            }
        }
        else
        {
            info.fsPath = plainFsPath;
        }

        info.exists = exists;
        info.isDir = isDir;
        info.isGzipped = useGz;
        if (info.isGzipped && info.logicalPath.endsWith(".gz"))
        {
            info.logicalPath = info.logicalPath.substring(0, info.logicalPath.length() - 3);
        }

#if LOG_LOCAL_LEVEL >= ESP_LOG_DEBUG
        ESP_LOGD(TAG, "[STATIC][FS] path=%s gz=%d exists=%d", info.fsPath.c_str(), info.isGzipped, info.exists);
#endif

        res.setStaticFileSystem(entry->fs);
        res.setStaticInfo(info);

        if (entry->staticHandler)
        {
            entry->staticHandler(info, req, res);
        }
    }

    void Server::setupStaticInfoFromMemory(HandlerEntry *entry, Request &req, Response &res, const String &normalizedUri, const String &relPath)
    {
        StaticInfo info;
        info.uri = normalizedUri;
        info.relPath = relPath;
        info.logicalPath = relPath;

        const bool requestGz = relPath.endsWith(".gz");
        String relBase = relPath;
        if (requestGz)
        {
            relBase = relPath.substring(0, relPath.length() - 3);
            if (relBase.isEmpty())
            {
                relBase = "/";
            }
        }
        const String gzRel = relBase + ".gz";

        int plainIndex = -1;
        int gzIndex = -1;
        for (size_t i = 0; i < entry->memCount; ++i)
        {
            const char *candidate = entry->memPaths[i];
            if (!candidate)
            {
                continue;
            }
            String candidateStr(candidate);
            if (candidateStr == relBase)
            {
                plainIndex = static_cast<int>(i);
            }
            else if (candidateStr == gzRel)
            {
                gzIndex = static_cast<int>(i);
            }
        }

        int chosenIndex = -1;
        bool gz = false;
        if (requestGz)
        {
            if (gzIndex >= 0)
            {
                chosenIndex = gzIndex;
                gz = true;
            }
        }
        else if (gzIndex >= 0)
        {
            chosenIndex = gzIndex;
            gz = true;
        }
        else if (plainIndex >= 0)
        {
            chosenIndex = plainIndex;
            gz = false;
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
            info.isGzipped = requestGz;
            res.clearStaticSource();
        }

#if LOG_LOCAL_LEVEL >= ESP_LOG_DEBUG
        ESP_LOGD(TAG, "[STATIC][MEM] path=%s gz=%d exists=%d", info.fsPath.c_str(), info.isGzipped, info.exists);
#endif

        res.setStaticInfo(info);

        if (entry->staticHandler)
        {
            entry->staticHandler(info, req, res);
        }
    }

    bool Server::ensureMethodHook(httpd_method_t method)
    {
        for (auto &hook : _methodHooks)
        {
            if (hook->method == method)
            {
                return true;
            }
        }

        auto hook = std::make_unique<MethodHook>();
        hook->method = method;
        hook->uriDef.uri = "/*";
        hook->uriDef.method = method;
        hook->uriDef.handler = &Server::handleDynamicHttpRequest;
        hook->uriDef.user_ctx = this;
        MethodHook *hookPtr = hook.get();
        _methodHooks.push_back(std::move(hook));
        if (_handle)
        {
            return registerMethodHook(hookPtr);
        }
        return true;
    }

    bool Server::registerMethodHook(MethodHook *hook)
    {
        if (!_handle || !hook)
        {
            return false;
        }
        hook->uriDef.user_ctx = this;
        const esp_err_t err = httpd_register_uri_handler(_handle, &hook->uriDef);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to register dynamic hook: %s", esp_err_to_name(err));
            return false;
        }
        hook->registered = true;
        return true;
    }

    esp_err_t Server::dispatchDynamic(httpd_req_t *req)
    {
        Request request(req);
        Response response(req);

        const String rawUri = request.uri();
#if LOG_LOCAL_LEVEL >= ESP_LOG_INFO
        {
            const String remote = clientAddress(req);
            ESP_LOGI(TAG, "[REQ] %s %s from %s", request.method().c_str(), rawUri.c_str(), remote.c_str());
        }
#endif

        String normalized;
        std::vector<String> pathSegments;
        if (!normalizeRoutePath(rawUri, normalized, pathSegments))
        {
            ESP_LOGW(TAG, "[RESP] 400 invalid path %s", rawUri.c_str());
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
            return ESP_OK;
        }

        std::vector<std::pair<String, String>> emptyParams;
        request.setPathInfo(normalized, emptyParams);

        httpd_method_t method = static_cast<httpd_method_t>(req->method);
        if (method == HTTP_GET)
        {
            if (tryHandleStaticRequest(request, response, method, rawUri, normalized))
            {
                return ESP_OK;
            }
        }

        DynamicRoute *bestRoute = nullptr;
        int bestScore = -1;
        std::vector<std::pair<String, String>> bestParams;
        std::vector<std::pair<String, String>> tempParams;

        for (auto &route : _dynamicRoutes)
        {
            if (route.method != method)
            {
                continue;
            }
            tempParams.clear();
            if (matchRoute(route, pathSegments, tempParams))
            {
                if (route.score > bestScore)
                {
                    bestScore = route.score;
                    bestRoute = &route;
                    bestParams = tempParams;
                }
            }
        }

        if (!bestRoute)
        {
            ESP_LOGI(TAG, "[404] %s %s", request.method().c_str(), normalized.c_str());
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
            return ESP_OK;
        }

        request.setPathInfo(normalized, bestParams);
#if LOG_LOCAL_LEVEL >= ESP_LOG_INFO
        {
            ESP_LOGI(TAG, "[ROUTE] %s -> %s", normalized.c_str(), bestRoute->pattern.c_str());
        }
#endif
#if LOG_LOCAL_LEVEL >= ESP_LOG_DEBUG
        {
            logParams(bestParams);
        }
#endif
        bestRoute->handler(request, response);
        return ESP_OK;
    }

    bool Server::tryHandleStaticRequest(Request &req, Response &res, httpd_method_t method, const String &rawPath, const String &normalizedPath)
    {
        if (method != HTTP_GET)
        {
            return false;
        }
        std::vector<std::pair<String, String>> emptyParams;
        for (auto &entryPtr : _handlers)
        {
            HandlerEntry *entry = entryPtr.get();
            if (!entry)
            {
                continue;
            }
            String relRaw;
            if (!extractRelativePath(rawPath, entry->uriPrefix, relRaw))
            {
                continue;
            }
            String relNormalized;
            if (!extractRelativePath(normalizedPath, entry->uriPrefix, relNormalized))
            {
                relNormalized = relRaw;
            }
            req.setPathInfo(normalizedPath, emptyParams);
            switch (entry->type)
            {
            case HandlerType::StaticFS:
#if LOG_LOCAL_LEVEL >= ESP_LOG_INFO
                ESP_LOGI(TAG, "[STATIC][FS] %s (rel=%s)", rawPath.c_str(), relRaw.c_str());
#endif
                setupStaticInfoFromFS(entry, req, res, normalizedPath, relNormalized);
                return true;
            case HandlerType::StaticMem:
#if LOG_LOCAL_LEVEL >= ESP_LOG_INFO
                ESP_LOGI(TAG, "[STATIC][MEM] %s (rel=%s)", rawPath.c_str(), relRaw.c_str());
#endif
                setupStaticInfoFromMemory(entry, req, res, normalizedPath, relNormalized);
                return true;
            }
        }
        return false;
    }

    bool Server::parseRoutePattern(const String &pattern, std::vector<RouteSegment> &segments, int &score)
    {
        segments.clear();
        score = 0;

        String working = pattern;
        int q = working.indexOf('?');
        if (q >= 0)
        {
            working = working.substring(0, q);
        }
        if (working.isEmpty())
        {
            working = "/";
        }
        if (!working.startsWith("/"))
        {
            working = "/" + working;
        }

        std::vector<String> rawSegments;
        String current;
        for (size_t i = 0; i < working.length(); ++i)
        {
            const char c = working.charAt(i);
            if (c == '/')
            {
                if (!current.isEmpty())
                {
                    rawSegments.push_back(current);
                    current.clear();
                }
                continue;
            }
            current += c;
        }
        if (!current.isEmpty())
        {
            rawSegments.push_back(current);
        }

        bool wildcardSeen = false;
        for (size_t i = 0; i < rawSegments.size(); ++i)
        {
            const String &token = rawSegments[i];
            RouteSegment segment;
            if (token.startsWith("*"))
            {
                if (wildcardSeen || i != rawSegments.size() - 1)
                {
                    return false;
                }
                String name = token.substring(1);
                if (name.isEmpty())
                {
                    return false;
                }
                segment.type = RouteSegment::Type::Wildcard;
                segment.value = name;
                wildcardSeen = true;
                score += 1;
            }
            else if (token.startsWith(":"))
            {
                String name = token.substring(1);
                if (name.isEmpty())
                {
                    return false;
                }
                segment.type = RouteSegment::Type::Param;
                segment.value = name;
                score += 2;
            }
            else
            {
                segment.type = RouteSegment::Type::Literal;
                segment.value = token;
                score += 3;
            }
            segments.push_back(segment);
        }

        return true;
    }

    bool Server::normalizeRoutePath(const String &raw, String &normalized, std::vector<String> &segments) const
    {
        String working = raw;
        int q = working.indexOf('?');
        if (q >= 0)
        {
            working = working.substring(0, q);
        }
        if (working.isEmpty())
        {
            working = "/";
        }
        if (!working.startsWith("/"))
        {
            working = "/" + working;
        }
        String decoded = urlDecode(working);
        segments.clear();
        String current;
        for (size_t i = 0; i < decoded.length(); ++i)
        {
            char c = decoded.charAt(i);
            if (c == '/')
            {
                if (!current.isEmpty())
                {
                    segments.push_back(current);
                    current.clear();
                }
                continue;
            }
            current += c;
        }
        if (!current.isEmpty())
        {
            segments.push_back(current);
        }

        normalized = "/";
        for (size_t i = 0; i < segments.size(); ++i)
        {
            normalized += segments[i];
            if (i + 1 < segments.size())
            {
                normalized += "/";
            }
        }
        return true;
    }

    bool Server::matchRoute(const DynamicRoute &route, const std::vector<String> &pathSegments, std::vector<std::pair<String, String>> &outParams) const
    {
        outParams.clear();
        size_t pathIndex = 0;
        for (size_t i = 0; i < route.segments.size(); ++i)
        {
            const auto &segment = route.segments[i];
            switch (segment.type)
            {
            case RouteSegment::Type::Literal:
                if (pathIndex >= pathSegments.size() || pathSegments[pathIndex] != segment.value)
                {
                    return false;
                }
                pathIndex++;
                break;
            case RouteSegment::Type::Param:
                if (pathIndex >= pathSegments.size())
                {
                    return false;
                }
                outParams.push_back({segment.value, pathSegments[pathIndex]});
                pathIndex++;
                break;
            case RouteSegment::Type::Wildcard:
            {
                String rest;
                for (size_t j = pathIndex; j < pathSegments.size(); ++j)
                {
                    if (j > pathIndex)
                    {
                        rest += "/";
                    }
                    rest += pathSegments[j];
                }
                outParams.push_back({segment.value, rest});
                pathIndex = pathSegments.size();
                break;
            }
            }
        }

        if (pathIndex != pathSegments.size())
        {
            return false;
        }
        return true;
    }

    String Server::urlDecode(const String &input) const
    {
        auto hexToInt = [](char c) -> int
        {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        };

        String out;
        out.reserve(input.length());
        for (size_t i = 0; i < input.length(); ++i)
        {
            char c = input.charAt(i);
            if (c == '%')
            {
                if (i + 2 < input.length())
                {
                    int hi = hexToInt(input.charAt(i + 1));
                    int lo = hexToInt(input.charAt(i + 2));
                    if (hi >= 0 && lo >= 0)
                    {
                        out += static_cast<char>((hi << 4) | lo);
                        i += 2;
                        continue;
                    }
                }
            }
            else if (c == '+')
            {
                out += ' ';
                continue;
            }
            out += c;
        }
        return out;
    }

#if LOG_LOCAL_LEVEL >= ESP_LOG_DEBUG
    void Server::logParams(const std::vector<std::pair<String, String>> &params) const
    {
        if (params.empty())
        {
            return;
        }
        String buffer;
        for (const auto &kv : params)
        {
            if (!buffer.isEmpty())
            {
                buffer += ", ";
            }
            buffer += kv.first;
            buffer += "=";
            buffer += kv.second;
        }
        ESP_LOGD(TAG, "[PARAMS] %s", buffer.c_str());
    }
#else
    void Server::logParams(const std::vector<std::pair<String, String>> &params) const
    {
        (void)params;
    }
#endif

#if LOG_LOCAL_LEVEL >= ESP_LOG_INFO
    String Server::clientAddress(httpd_req_t *req) const
    {
        int sock = httpd_req_to_sockfd(req);
        if (sock < 0)
        {
            return String("-");
        }
        sockaddr_in addr;
        socklen_t len = sizeof(addr);
        memset(&addr, 0, sizeof(addr));
        if (getpeername(sock, reinterpret_cast<sockaddr *>(&addr), &len) != 0 || addr.sin_family != AF_INET)
        {
            return String("-");
        }
        char buffer[IP4ADDR_STRLEN_MAX] = {0};
        ip4addr_ntoa_r(reinterpret_cast<const ip4_addr_t *>(&addr.sin_addr), buffer, sizeof(buffer));
        return String(buffer);
    }
#else
    String Server::clientAddress(httpd_req_t *req) const
    {
        (void)req;
        return String("-");
    }
#endif

} // namespace EspHttpServer
