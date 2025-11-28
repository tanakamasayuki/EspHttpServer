#include "EspHttpServer.h"

#include <esp_log.h>
#include <esp_system.h>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <lwip/sockets.h>
#include <lwip/ip4_addr.h>
#include <memory>
#include <new>

#ifdef CORE_DEBUG_LEVEL
#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL CORE_DEBUG_LEVEL
#endif

namespace EspHttpServer
{

    size_t Request::_maxFormSize = 8 * 1024;

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
            constexpr size_t kChunkSize = 1024;
            std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[kChunkSize]);
            if (!buffer)
            {
                ESP_LOGE(TAG, "Failed to allocate stream buffer");
                file.close();
                return false;
            }
            while (file.available())
            {
                const size_t readLen = file.read(buffer.get(), kChunkSize);
                if (readLen == 0)
                {
                    break;
                }
                if (httpd_resp_send_chunk(raw, reinterpret_cast<const char *>(buffer.get()), readLen) != ESP_OK)
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

        bool containsControlChars(const String &text)
        {
            for (size_t i = 0; i < text.length(); ++i)
            {
                const unsigned char c = static_cast<unsigned char>(text.charAt(i));
                if (c < 0x20 || c == 0x7f)
                {
                    return true;
                }
            }
            return false;
        }

        void trimSpaces(String &text)
        {
            while (text.length() > 0 && isspace(static_cast<unsigned char>(text.charAt(0))))
            {
                text.remove(0, 1);
            }
            while (text.length() > 0 && isspace(static_cast<unsigned char>(text.charAt(text.length() - 1))))
            {
                text.remove(text.length() - 1, 1);
            }
        }

        bool parseUrlEncodedInternal(const String &input, std::vector<std::pair<String, String>> &out)
        {
            auto decode = [](const String &in, String &decoded) -> bool
            {
                decoded.clear();
                decoded.reserve(in.length());
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
                for (size_t i = 0; i < in.length(); ++i)
                {
                    char c = in.charAt(i);
                    if (c == '+')
                    {
                        decoded += ' ';
                        continue;
                    }
                    if (c == '%' && i + 2 < in.length())
                    {
                        int hi = hexToInt(in.charAt(i + 1));
                        int lo = hexToInt(in.charAt(i + 2));
                        if (hi >= 0 && lo >= 0)
                        {
                            decoded += static_cast<char>((hi << 4) | lo);
                            i += 2;
                            continue;
                        }
                        return false;
                    }
                    decoded += c;
                }
                return true;
            };

            size_t start = 0;
            while (start <= input.length())
            {
                int amp = input.indexOf('&', start);
                if (amp < 0)
                {
                    amp = input.length();
                }
                String token = input.substring(start, amp);
                int eq = token.indexOf('=');
                String rawKey;
                String rawVal;
                if (eq >= 0)
                {
                    rawKey = token.substring(0, eq);
                    rawVal = token.substring(eq + 1);
                }
                else
                {
                    rawKey = token;
                    rawVal = String();
                }
                String keyDecoded;
                String valDecoded;
                if (!rawKey.isEmpty() && decode(rawKey, keyDecoded) && decode(rawVal, valDecoded) && !containsControlChars(keyDecoded) && !containsControlChars(valDecoded))
                {
                    out.push_back({keyDecoded, valDecoded});
                }
                start = static_cast<size_t>(amp) + 1;
                if (amp == static_cast<int>(input.length()))
                {
                    break;
                }
            }
            return true;
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

    bool Request::decodeComponent(const String &input, String &output)
    {
        output.clear();
        output.reserve(input.length());
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

        for (size_t i = 0; i < input.length(); ++i)
        {
            char c = input.charAt(i);
            if (c == '+')
            {
                output += ' ';
                continue;
            }
            if (c == '%' && i + 2 < input.length())
            {
                int hi = hexToInt(input.charAt(i + 1));
                int lo = hexToInt(input.charAt(i + 2));
                if (hi >= 0 && lo >= 0)
                {
                    output += static_cast<char>((hi << 4) | lo);
                    i += 2;
                    continue;
                }
                return false;
            }
            output += c;
        }
        return true;
    }

    bool Request::ensureQueryParsed() const
    {
        if (_queryParsed)
        {
            return true;
        }
        _queryParsed = true;
        const String fullUri = uri();
        int qPos = fullUri.indexOf('?');
        if (qPos < 0 || qPos + 1 >= fullUri.length())
        {
            return true;
        }
        String queryStr = fullUri.substring(qPos + 1);
        parseUrlEncodedInternal(queryStr, _queryParams);
        return true;
    }

    bool Request::ensureFormParsed() const
    {
        if (_formParsed)
        {
            return !_formOverflow;
        }
        _formParsed = true;
        _formOverflow = false;
        if (!_raw)
        {
            return true;
        }

        size_t contentLength = _raw->content_len;
        if (contentLength == 0)
        {
            return true;
        }

        char ctypeBuf[64] = {0};
        String contentType;
        size_t ctypeLen = httpd_req_get_hdr_value_len(_raw, "Content-Type");
        if (ctypeLen > 0 && ctypeLen < sizeof(ctypeBuf))
        {
            if (httpd_req_get_hdr_value_str(_raw, "Content-Type", ctypeBuf, sizeof(ctypeBuf)) == ESP_OK)
            {
                contentType = String(ctypeBuf);
            }
        }
        if (!isUrlEncodedContentType(contentType))
        {
            return true;
        }

        if (contentLength > _maxFormSize)
        {
            _formOverflow = true;
            httpd_resp_send_err(_raw, HTTPD_400_BAD_REQUEST, "Form too large");
            return false;
        }

        std::unique_ptr<char[]> body(new (std::nothrow) char[contentLength + 1]);
        if (!body)
        {
            _formOverflow = true;
            return false;
        }
        size_t received = 0;
        while (received < contentLength)
        {
            const size_t toRead = std::min(static_cast<size_t>(1024), contentLength - received);
            int ret = httpd_req_recv(_raw, body.get() + received, toRead);
            if (ret <= 0)
            {
                _formOverflow = true;
                return false;
            }
            received += static_cast<size_t>(ret);
        }
        body[contentLength] = '\0';
        String formText(body.get());
        parseUrlEncodedInternal(formText, _formParams);
        return true;
    }

    bool Request::isUrlEncodedContentType(const String &contentType)
    {
        if (contentType.isEmpty())
        {
            return false;
        }
        String lower = contentType;
        lower.toLowerCase();
        return lower.startsWith("application/x-www-form-urlencoded");
    }

    bool Request::extractBoundary(const String &contentType, String &boundaryOut)
    {
        boundaryOut.clear();
        if (contentType.isEmpty())
        {
            return false;
        }
        String lower = contentType;
        lower.toLowerCase();
        int pos = lower.indexOf("boundary=");
        if (pos < 0)
        {
            return false;
        }
        String rest = contentType.substring(pos + 9);
        rest.trim();
        if (rest.startsWith("\""))
        {
            int endQuote = rest.indexOf('"', 1);
            if (endQuote > 1)
            {
                boundaryOut = rest.substring(1, endQuote);
            }
        }
        else
        {
            int semi = rest.indexOf(';');
            if (semi >= 0)
            {
                boundaryOut = rest.substring(0, semi);
            }
            else
            {
                boundaryOut = rest;
            }
        }
        boundaryOut.trim();
        return !boundaryOut.isEmpty();
    }

    bool Request::ensureMultipartParsed() const
    {
        if (_multipartParsed)
        {
            return !_multipartOverflow;
        }
        _multipartParsed = true;
        _multipartOverflow = false;
        if (!_raw)
        {
            return true;
        }

        size_t contentLength = _raw->content_len;
        if (contentLength == 0)
        {
            return true;
        }

        char ctypeBuf[128] = {0};
        String contentType;
        size_t ctypeLen = httpd_req_get_hdr_value_len(_raw, "Content-Type");
        if (ctypeLen > 0 && ctypeLen < sizeof(ctypeBuf))
        {
            if (httpd_req_get_hdr_value_str(_raw, "Content-Type", ctypeBuf, sizeof(ctypeBuf)) == ESP_OK)
            {
                contentType = String(ctypeBuf);
            }
        }
        String boundary;
        if (!extractBoundary(contentType, boundary) || boundary.isEmpty())
        {
            return true;
        }

        if (contentLength > _maxFormSize)
        {
            _multipartOverflow = true;
            httpd_resp_send_err(_raw, HTTPD_400_BAD_REQUEST, "Multipart too large");
            return false;
        }

        std::unique_ptr<char[]> body(new (std::nothrow) char[contentLength + 1]);
        if (!body)
        {
            _multipartOverflow = true;
            return false;
        }
        size_t received = 0;
        while (received < contentLength)
        {
            const size_t toRead = std::min(static_cast<size_t>(1024), contentLength - received);
            int ret = httpd_req_recv(_raw, body.get() + received, toRead);
            if (ret <= 0)
            {
                _multipartOverflow = true;
                return false;
            }
            received += static_cast<size_t>(ret);
        }
        body[contentLength] = '\0';
        String payload(body.get());

        const String boundaryToken = "--" + boundary;
        size_t pos = 0;
        while (true)
        {
            int start = payload.indexOf(boundaryToken, pos);
            if (start < 0)
            {
                break;
            }
            start += boundaryToken.length();
            if (payload.startsWith("--", start))
            {
                break; // closing boundary
            }
            if (payload.startsWith("\r\n", start))
            {
                start += 2;
            }
            int headerEnd = payload.indexOf("\r\n\r\n", start);
            if (headerEnd < 0)
            {
                break;
            }
            String headerBlock = payload.substring(start, headerEnd);
            int nextBoundary = payload.indexOf("\r\n" + boundaryToken, headerEnd + 4);
            if (nextBoundary < 0)
            {
                break;
            }
            String contentData = payload.substring(headerEnd + 4, nextBoundary);

            MultipartField field;
            field.info.size = contentData.length();

            int dispPos = headerBlock.indexOf("Content-Disposition:");
            if (dispPos >= 0)
            {
                String disp = headerBlock.substring(dispPos);
                int namePos = disp.indexOf("name=");
                if (namePos >= 0)
                {
                    int firstQuote = disp.indexOf('"', namePos);
                    int secondQuote = disp.indexOf('"', firstQuote + 1);
                    if (firstQuote >= 0 && secondQuote > firstQuote)
                    {
                        field.info.name = disp.substring(firstQuote + 1, secondQuote);
                    }
                }
                int filePos = disp.indexOf("filename=");
                if (filePos >= 0)
                {
                    int fq = disp.indexOf('"', filePos);
                    int sq = disp.indexOf('"', fq + 1);
                    if (fq >= 0 && sq > fq)
                    {
                        field.info.filename = disp.substring(fq + 1, sq);
                    }
                }
            }

            int ctypePos = headerBlock.indexOf("Content-Type:");
            if (ctypePos >= 0)
            {
                int lineEnd = headerBlock.indexOf("\r\n", ctypePos);
                String ctLine = (lineEnd > ctypePos) ? headerBlock.substring(ctypePos + 13, lineEnd) : headerBlock.substring(ctypePos + 13);
                ctLine.trim();
                field.info.contentType = ctLine;
            }

            field.data = contentData;
            _multipartFields.push_back(field);
            pos = static_cast<size_t>(nextBoundary + 2); // skip leading CRLF before boundary
        }
        return true;
    }

    bool Request::parseUrlEncoded(const String &text, std::vector<std::pair<String, String>> &out) const
    {
        return parseUrlEncodedInternal(text, out);
    }

    bool Request::hasQueryParam(const String &name) const
    {
        if (name.isEmpty())
        {
            return false;
        }
        ensureQueryParsed();
        for (auto it = _queryParams.rbegin(); it != _queryParams.rend(); ++it)
        {
            if (it->first == name)
            {
                return true;
            }
        }
        return false;
    }

    String Request::queryParam(const String &name) const
    {
        if (name.isEmpty())
        {
            return String();
        }
        ensureQueryParsed();
        for (auto it = _queryParams.rbegin(); it != _queryParams.rend(); ++it)
        {
            if (it->first == name)
            {
                return it->second;
            }
        }
        return String();
    }

    void Request::forEachQueryParam(std::function<bool(const String &name, const String &value)> cb) const
    {
        if (!cb)
        {
            return;
        }
        ensureQueryParsed();
        for (const auto &kv : _queryParams)
        {
            if (!cb(kv.first, kv.second))
            {
                break;
            }
        }
    }

    bool Request::hasMultipartField(const String &name) const
    {
        if (name.isEmpty())
        {
            return false;
        }
        if (!ensureMultipartParsed())
        {
            return false;
        }
        for (auto it = _multipartFields.rbegin(); it != _multipartFields.rend(); ++it)
        {
            if (it->info.name == name)
            {
                return true;
            }
        }
        return false;
    }

    String Request::multipartField(const String &name) const
    {
        if (name.isEmpty())
        {
            return String();
        }
        if (!ensureMultipartParsed())
        {
            return String();
        }
        for (auto it = _multipartFields.rbegin(); it != _multipartFields.rend(); ++it)
        {
            if (it->info.name == name)
            {
                return it->data;
            }
        }
        return String();
    }

    void Request::onMultipart(MultipartFieldHandler handler) const
    {
        if (!handler)
        {
            return;
        }
        if (!ensureMultipartParsed())
        {
            return;
        }
        for (const auto &field : _multipartFields)
        {
            struct MemoryStream : public Stream
            {
                const String &data;
                size_t pos = 0;
                explicit MemoryStream(const String &d) : data(d) {}
                int available() override { return static_cast<int>(data.length() - pos); }
                int read() override
                {
                    if (pos >= data.length())
                        return -1;
                    return static_cast<unsigned char>(data.charAt(pos++));
                }
                int peek() override
                {
                    if (pos >= data.length())
                        return -1;
                    return static_cast<unsigned char>(data.charAt(pos));
                }
                size_t readBytes(char *buffer, size_t len) override
                {
                    size_t remain = data.length() - pos;
                    size_t toCopy = std::min(remain, len);
                    memcpy(buffer, data.c_str() + pos, toCopy);
                    pos += toCopy;
                    return toCopy;
                }
                void flush() override {}
                size_t write(uint8_t) override { return 0; }
            } stream(field.data);

            if (!handler(field.info, stream))
            {
                break;
            }
        }
    }

    bool Request::hasFormParam(const String &name) const
    {
        if (name.isEmpty())
        {
            return false;
        }
        if (!ensureFormParsed())
        {
            return false;
        }
        for (auto it = _formParams.rbegin(); it != _formParams.rend(); ++it)
        {
            if (it->first == name)
            {
                return true;
            }
        }
        return false;
    }

    String Request::formParam(const String &name) const
    {
        if (name.isEmpty())
        {
            return String();
        }
        if (!ensureFormParsed())
        {
            return String();
        }
        for (auto it = _formParams.rbegin(); it != _formParams.rend(); ++it)
        {
            if (it->first == name)
            {
                return it->second;
            }
        }
        return String();
    }

    void Request::forEachFormParam(std::function<bool(const String &name, const String &value)> cb) const
    {
        if (!cb)
        {
            return;
        }
        if (!ensureFormParsed())
        {
            return;
        }
        for (const auto &kv : _formParams)
        {
            if (!cb(kv.first, kv.second))
            {
                break;
            }
        }
    }

    void Request::setMaxFormSize(size_t bytes)
    {
        _maxFormSize = bytes;
    }

    bool Request::ensureCookiesParsed() const
    {
        if (_cookiesParsed)
        {
            return true;
        }
        _cookiesParsed = true;
        if (!_raw)
        {
            return false;
        }

        size_t len = httpd_req_get_hdr_value_len(_raw, "Cookie");
        if (len == 0)
        {
            return true;
        }
        std::unique_ptr<char[]> buffer(new (std::nothrow) char[len + 1]);
        if (!buffer)
        {
            ESP_LOGE(TAG, "cookie buffer alloc failed");
            return false;
        }
        if (httpd_req_get_hdr_value_str(_raw, "Cookie", buffer.get(), len + 1) != ESP_OK)
        {
            ESP_LOGW(TAG, "failed to read Cookie header");
            return false;
        }

        auto processToken = [&](const String &token) {
            String working = token;
            trimSpaces(working);
            if (working.isEmpty())
            {
                return;
            }
            int eq = working.indexOf('=');
            if (eq < 0)
            {
                return;
            }
            String name = working.substring(0, eq);
            String value = working.substring(eq + 1);
            trimSpaces(name);
            trimSpaces(value);
            if (name.isEmpty() || containsControlChars(name) || containsControlChars(value))
            {
                return;
            }
            _cookies.push_back({name, value});
        };

        String token;
        for (size_t i = 0; i < len; ++i)
        {
            const char c = buffer[i];
            if (c == ';')
            {
                processToken(token);
                token.clear();
                continue;
            }
            token += c;
        }
        processToken(token);
        return true;
    }

    bool Request::hasCookie(const String &name) const
    {
        if (name.isEmpty())
        {
            return false;
        }
        ensureCookiesParsed();
        for (const auto &kv : _cookies)
        {
            if (kv.first == name)
            {
                return true;
            }
        }
        return false;
    }

    String Request::cookie(const String &name) const
    {
        if (name.isEmpty())
        {
            return String();
        }
        ensureCookiesParsed();
        for (const auto &kv : _cookies)
        {
            if (kv.first == name)
            {
                return kv.second;
            }
        }
        return String();
    }

    void Request::forEachCookie(std::function<bool(const String &name, const String &value)> cb) const
    {
        if (!cb)
        {
            return;
        }
        ensureCookiesParsed();
        for (const auto &kv : _cookies)
        {
            if (!cb(kv.first, kv.second))
            {
                break;
            }
        }
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
        _lastStatusCode = 0;
        _requestContext = nullptr;
        _responseCommitted = false;
        _setCookieBuffers.clear();
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

    void Response::setErrorRenderer(ErrorRenderer handler)
    {
        _errorRenderer = std::move(handler);
    }

    void Response::clearErrorRenderer()
    {
        _errorRenderer = nullptr;
    }

    void Response::send(int code, const char *type, const uint8_t *data, size_t len)
    {
        if (!_raw)
            return;
        _lastStatusCode = code;
        const String typeStr = type ? String(type) : String();
        const bool htmlEligible = isHtmlMime(typeStr);
        const bool needsProcessing = htmlEligible && (_templateHandler || (_headInjectionPtr && _headInjectionPtr[0]));

        httpd_resp_set_type(_raw, type);
        httpd_resp_set_status(_raw, statusString(code));
        markCommitted();

        if (needsProcessing)
        {
            StaticInputStream stream(data, len);
            if (!streamHtmlFromSource(stream))
            {
                ESP_LOGE(TAG, "[RESP] %d html processing failed", code);
                sendError(HTTPD_500_INTERNAL_SERVER_ERROR);
                return;
            }
            ESP_LOGI(TAG, "[RESP][TPL] %d %s processed", code, type ? type : "-", len);
            return;
        }

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
        _lastStatusCode = code;
        httpd_resp_set_type(_raw, type);
        httpd_resp_set_status(_raw, statusString(code));
        ESP_LOGI(TAG, "[RESP] %d %s (chunked)", code, type ? type : "-");
        markCommitted();
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
        ESP_LOGI(TAG, "[RESP] chunked end (%d)", _lastStatusCode);
    }

    void Response::sendStatic()
    {
        if (!_raw)
            return;

        if (_staticSource == StaticSourceType::None)
        {
            ESP_LOGE(TAG, "Static source missing");
            sendError(HTTPD_500_INTERNAL_SERVER_ERROR);
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
            sendError(HTTPD_404_NOT_FOUND);
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
        constexpr int kStaticStatusCode = 200;
        _lastStatusCode = kStaticStatusCode;
        markCommitted();

        const char *sourceLabel = "NONE";
        const char *originPath = "-";
        if (_staticSource == StaticSourceType::FileSystem)
        {
            sourceLabel = "FS";
            originPath = _staticInfo.fsPath.c_str();
        }
        else if (_staticSource == StaticSourceType::Memory)
        {
            sourceLabel = "MEM";
        }
        ESP_LOGI(TAG,
                 "[RESP][STATIC][%s] %d %s (%s) origin=%s",
                 sourceLabel,
                 kStaticStatusCode,
                 logicalPath.c_str(),
                 _staticInfo.isGzipped ? "gzip" : "plain",
                 originPath);

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
                sendError(HTTPD_500_INTERNAL_SERVER_ERROR);
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
            sendError(HTTPD_500_INTERNAL_SERVER_ERROR);
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

    void Response::sendError(int status)
    {
        if (!_raw)
            return;
        _chunked = false;
        _lastStatusCode = status;
        httpd_resp_set_status(_raw, statusString(status));
        ESP_LOGI(TAG, "[RESP][ERR] %d", status);
        markCommitted();
        if (_errorRenderer && _requestContext)
        {
            _errorRenderer(status, *_requestContext, *this);
            return;
        }
        const char *message = defaultErrorMessage(status);
        httpd_resp_set_type(_raw, "text/plain");
        httpd_resp_send(_raw, message, strlen(message));
    }

    bool Response::committed() const
    {
        return _responseCommitted;
    }

    void Response::redirect(const char *location, int status)
    {
        if (!_raw)
            return;
        _lastStatusCode = status;
        const char *statusStr = statusString(status);
        httpd_resp_set_status(_raw, statusStr);
        httpd_resp_set_hdr(_raw, "Location", location);
        httpd_resp_send(_raw, nullptr, 0);
        ESP_LOGI(TAG, "[RESP] %d redirect -> %s", status, location);
        markCommitted();
    }

    void Response::setStaticInfo(const StaticInfo &info)
    {
        _staticInfo = info;
    }

    void Response::setCookie(const Cookie &cookie)
    {
        if (!_raw)
        {
            return;
        }
        if (_responseCommitted)
        {
            ESP_LOGW(TAG, "Set-Cookie after commit ignored");
            return;
        }
        if (cookie.name.isEmpty() || containsControlChars(cookie.name) || containsControlChars(cookie.value))
        {
            ESP_LOGW(TAG, "invalid cookie skipped");
            return;
        }

        Cookie::SameSite sameSite = cookie.sameSite;
        bool secure = cookie.secure;
        if (sameSite == Cookie::SameSite::None && !secure)
        {
            secure = true;
        }

        String header;
        header.reserve(cookie.name.length() + cookie.value.length() + 64);
        header += cookie.name;
        header += "=";
        header += cookie.value;

        if (!cookie.path.isEmpty())
        {
            header += "; Path=";
            header += cookie.path;
        }
        if (!cookie.domain.isEmpty())
        {
            header += "; Domain=";
            header += cookie.domain;
        }
        if (cookie.maxAge >= 0)
        {
            header += "; Max-Age=";
            header += String(cookie.maxAge);
        }
        if (secure)
        {
            header += "; Secure";
        }
        if (cookie.httpOnly)
        {
            header += "; HttpOnly";
        }
        header += "; SameSite=";
        switch (sameSite)
        {
        case Cookie::SameSite::None:
            header += "None";
            break;
        case Cookie::SameSite::Strict:
            header += "Strict";
            break;
        case Cookie::SameSite::Lax:
        default:
            header += "Lax";
            break;
        }

        size_t len = header.length();
        std::unique_ptr<char[]> buf(new (std::nothrow) char[len + 1]);
        if (!buf)
        {
            ESP_LOGE(TAG, "cookie header alloc failed");
            return;
        }
        memcpy(buf.get(), header.c_str(), len + 1);
        httpd_resp_set_hdr(_raw, "Set-Cookie", buf.get());
        _setCookieBuffers.push_back(std::move(buf));
    }

    void Response::clearCookie(const String &name, const String &path)
    {
        Cookie c;
        c.name = name;
        c.value = "";
        c.path = path;
        c.maxAge = 0;
        setCookie(c);
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

    void Response::setRequestContext(Request *req)
    {
        _requestContext = req;
    }

    void Response::markCommitted()
    {
        _responseCommitted = true;
    }

    const char *Response::defaultErrorMessage(int status)
    {
        switch (status)
        {
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 500:
            return "Internal Server Error";
        case 503:
            return "Service Unavailable";
        default:
            return "Error";
        }
    }

    ErrorRenderer Response::_errorRenderer;

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

    namespace
    {
        String defaultSessionId(size_t idBytes)
        {
            const size_t bytes = idBytes > 0 ? idBytes : 16;
            String out;
            out.reserve(bytes * 2);
            static const char kHex[] = "0123456789abcdef";
            for (size_t i = 0; i < bytes; ++i)
            {
                uint8_t b = static_cast<uint8_t>(esp_random() & 0xFF);
                out += kHex[(b >> 4) & 0x0F];
                out += kHex[b & 0x0F];
            }
            return out;
        }

        bool defaultValidateSessionId(const String &id, size_t idBytes)
        {
            if (id.isEmpty())
            {
                return false;
            }
            size_t minLen = idBytes ? (idBytes * 2) / 3 : 8;
            if (minLen < 8)
            {
                minLen = 8;
            }
            if (id.length() < minLen)
            {
                return false;
            }
            for (size_t i = 0; i < id.length(); ++i)
            {
                const unsigned char c = static_cast<unsigned char>(id.charAt(i));
                if (!(isalnum(c) || c == '-' || c == '_' || c == '.'))
                {
                    return false;
                }
            }
            return true;
        }

        Cookie buildSessionCookie(const SessionConfig &cfg, const String &value)
        {
            Cookie c;
            c.name = cfg.cookieName.isEmpty() ? String("sid") : cfg.cookieName;
            c.value = value;
            c.path = cfg.path.isEmpty() ? String("/") : cfg.path;
            c.maxAge = cfg.maxAgeSeconds;
            c.secure = cfg.secure;
            c.httpOnly = cfg.httpOnly;
            c.sameSite = cfg.sameSite;
            return c;
        }
    } // namespace

    SessionInfo beginSession(Request &req, Response &res, const SessionConfig &cfg)
    {
        SessionInfo info;

        const String cookieName = cfg.cookieName.isEmpty() ? String("sid") : cfg.cookieName;
        const String existing = req.cookie(cookieName);

        bool valid = false;
        if (!existing.isEmpty())
        {
            if (cfg.validate)
            {
                valid = cfg.validate(existing);
            }
            else
            {
                valid = defaultValidateSessionId(existing, cfg.idBytes);
            }
        }

        if (valid)
        {
            info.id = existing;
            info.isNew = false;
        }
        else
        {
            info.id = cfg.generate ? cfg.generate() : defaultSessionId(cfg.idBytes);
            if (info.id.isEmpty())
            {
                info.id = defaultSessionId(cfg.idBytes);
            }
            info.isNew = true;
        }
        info.rotated = false;

        if (!info.id.isEmpty() && (info.isNew || cfg.maxAgeSeconds >= 0))
        {
            Cookie c = buildSessionCookie(cfg, info.id);
            res.setCookie(c);
        }

        return info;
    }

    SessionInfo rotateSession(SessionInfo &cur, Response &res, const SessionConfig &cfg)
    {
        SessionInfo updated = cur;
        const String oldId = cur.id;
        updated.id = cfg.generate ? cfg.generate() : defaultSessionId(cfg.idBytes);
        if (updated.id.isEmpty())
        {
            updated.id = defaultSessionId(cfg.idBytes);
        }
        updated.rotated = true;
        updated.isNew = false;

        if (!updated.id.isEmpty())
        {
            Cookie c = buildSessionCookie(cfg, updated.id);
            res.setCookie(c);
        }

        if (cfg.onRotate && !oldId.isEmpty() && !updated.id.isEmpty())
        {
            cfg.onRotate(oldId, updated.id);
        }

        cur = updated;
        return updated;
    }

    void touchSessionCookie(SessionInfo &cur, Response &res, const SessionConfig &cfg)
    {
        if (cur.id.isEmpty() || cfg.maxAgeSeconds < 0)
        {
            return;
        }
        Cookie c = buildSessionCookie(cfg, cur.id);
        res.setCookie(c);
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

    void Server::onNotFound(RouteHandler handler)
    {
        _notFoundHandler = std::move(handler);
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
            res.sendError(HTTPD_500_INTERNAL_SERVER_ERROR);
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

        if (exists && isDir)
        {
            String dirRel = ensureLeadingSlash(relBase);
            if (!dirRel.endsWith("/"))
            {
                dirRel += "/";
            }
            static const char *kIndexCandidates[] = {"index.html", "index.htm"};
            bool foundIndex = false;
            for (const char *candidateName : kIndexCandidates)
            {
                if (!candidateName)
                {
                    continue;
                }
                String candidateRel = dirRel + candidateName;
                const String candidatePlain = joinFsPath(entry->basePath, candidateRel);
                const String candidateGz = candidatePlain + ".gz";
                if (entry->fs->exists(candidateGz))
                {
                    info.fsPath = candidateGz;
                    info.logicalPath = ensureLeadingSlash(candidateRel);
                    exists = true;
                    useGz = true;
                    isDir = false;
                    foundIndex = true;
                    break;
                }
                if (entry->fs->exists(candidatePlain))
                {
                    info.fsPath = candidatePlain;
                    info.logicalPath = ensureLeadingSlash(candidateRel);
                    exists = true;
                    useGz = false;
                    isDir = false;
                    foundIndex = true;
                    break;
                }
            }
            if (!foundIndex)
            {
                exists = false;
                isDir = false;
            }
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
        if (!res.committed())
        {
            if (info.exists)
            {
                res.sendStatic();
            }
            else
            {
                res.sendError(HTTPD_404_NOT_FOUND);
            }
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
        relBase = ensureLeadingSlash(relBase);
        const String gzRel = relBase + ".gz";
        String dirPrefix = relBase;
        if (!dirPrefix.endsWith("/"))
        {
            dirPrefix += "/";
        }

        bool hasChildPrefix = false;
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
            if (plainIndex < 0 && candidateStr == relBase)
            {
                plainIndex = static_cast<int>(i);
            }
            else if (gzIndex < 0 && candidateStr == gzRel)
            {
                gzIndex = static_cast<int>(i);
            }
            if (!hasChildPrefix && candidateStr.startsWith(dirPrefix))
            {
                hasChildPrefix = true;
            }
        }

        auto findIndexByPath = [&](const String &target) -> int
        {
            if (target.isEmpty())
            {
                return -1;
            }
            for (size_t i = 0; i < entry->memCount; ++i)
            {
                const char *candidate = entry->memPaths[i];
                if (!candidate)
                {
                    continue;
                }
                if (target == candidate)
                {
                    return static_cast<int>(i);
                }
            }
            return -1;
        };

        bool directoryHint = relPath.endsWith("/") || hasChildPrefix;
        if (plainIndex < 0 && gzIndex < 0 && directoryHint)
        {
            static const char *kIndexCandidates[] = {"index.html", "index.htm"};
            for (const char *candidateName : kIndexCandidates)
            {
                if (!candidateName)
                {
                    continue;
                }
                String candidateRel = dirPrefix + candidateName;
                int gzCandidate = findIndexByPath(candidateRel + ".gz");
                if (gzCandidate >= 0)
                {
                    gzIndex = gzCandidate;
                    plainIndex = -1;
                    info.logicalPath = candidateRel;
                    relBase = candidateRel;
                    break;
                }
                int plainCandidate = findIndexByPath(candidateRel);
                if (plainCandidate >= 0)
                {
                    plainIndex = plainCandidate;
                    info.logicalPath = candidateRel;
                    relBase = candidateRel;
                    break;
                }
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
        if (!res.committed())
        {
            if (info.exists)
            {
                res.sendStatic();
            }
            else
            {
                res.sendError(HTTPD_404_NOT_FOUND);
            }
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
        response.setRequestContext(&request);

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
            response.sendError(HTTPD_400_BAD_REQUEST);
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
            if (_notFoundHandler)
            {
                _notFoundHandler(request, response);
                if (!response.committed())
                {
                    response.sendError(HTTPD_404_NOT_FOUND);
                }
                return ESP_OK;
            }
            ESP_LOGI(TAG, "[404] %s %s", request.method().c_str(), normalized.c_str());
            response.sendError(HTTPD_404_NOT_FOUND);
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
        if (!response.committed())
        {
            response.sendError(HTTPD_500_INTERNAL_SERVER_ERROR);
        }
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
