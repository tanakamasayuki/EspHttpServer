#include <WiFi.h>
#include <EspHttpServer.h>
#include <stdlib.h>

EspHttpServer::Server server;

#if __has_include("arduino_secrets.h")
#include "arduino_secrets.h"
#else
#define WIFI_SSID "YourSSID"     // Enter your Wi-Fi SSID here / Wi-FiのSSIDを入力
#define WIFI_PASS "YourPassword" // Enter your Wi-Fi password here / Wi-Fiのパスワードを入力
#endif

// en: Session slots (fixed 16). Oldest access gets evicted when full.
// ja: セッションスロット（固定16件）。満杯なら最も古いアクセスを上書きします。
struct SessionSlot
{
    bool used = false;
    String id;
    uint32_t hits = 0;
    uint64_t lastAccessMs = 0;
};

constexpr size_t kMaxSessions = 16;
SessionSlot g_sessions[kMaxSessions];

// en: HTML template placeholders: sessionId, hits, slot, replaced.
// ja: テンプレート用 HTML（sessionId / hits / slot を差し込み）。
const char kHtml[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Session demo</title>
  <style>
    body{font-family:system-ui,-apple-system,"Noto Sans JP",sans-serif;padding:24px;line-height:1.6;}
    code{background:#f1f5f9;padding:2px 6px;border-radius:6px;}
    section{margin-bottom:16px;}
    .note{color:#475569;}
  </style>
</head>
<body>
  <h1>シンプルセッションの例</h1>
  <section class="note">
    <p>セッションIDは Cookie <code>sid</code> で払い出し、スロットは16個固定です。空きが無ければ最も古いセッションを上書きします。</p>
  </section>
  <section>
    <div>セッションID: <code>{{sessionId}}</code></div>
    <div>スロット番号: <code>{{slot}}</code> / {{capacity}}</div>
    <div>このセッションでのアクセス回数: <code>{{hits}}</code></div>
  </section>
  <section>
    <form action="/rotate" method="POST" style="margin-bottom:8px"><button type="submit">セッションIDを変更 (rotate)</button></form>
    <form action="/clear" method="POST"><button type="submit">このセッションをクリア</button></form>
  </section>
</body>
</html>
)HTML";

// en: Fixed session config (sid cookie, 7-day TTL).
// ja: 固定のセッション設定（sid Cookie, 7日TTL）。
EspHttpServer::SessionConfig buildSessionConfig()
{
    EspHttpServer::SessionConfig cfg;
    cfg.cookieName = "sid";
    cfg.maxAgeSeconds = 7 * 24 * 60 * 60;
    cfg.path = "/";
    cfg.httpOnly = true;
    cfg.sameSite = EspHttpServer::Cookie::SameSite::Lax;
    return cfg;
}

// en: Find a slot by ID, return index or -1.
// ja: ID でスロットを検索してインデックスを返す（無ければ -1）。
int findSlotById(const String &id)
{
    for (size_t i = 0; i < kMaxSessions; ++i)
    {
        if (g_sessions[i].used && g_sessions[i].id == id)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// en: Get a free slot or the oldest slot index.
// ja: 空きスロットか最古のスロットのインデックスを返す。
int acquireSlotIndex()
{
    int freeIndex = -1;
    uint64_t oldestTime = UINT64_MAX;
    int oldestIndex = -1;
    for (size_t i = 0; i < kMaxSessions; ++i)
    {
        if (!g_sessions[i].used)
        {
            freeIndex = static_cast<int>(i);
            break;
        }
        if (g_sessions[i].lastAccessMs < oldestTime)
        {
            oldestTime = g_sessions[i].lastAccessMs;
            oldestIndex = static_cast<int>(i);
        }
    }
    if (freeIndex >= 0)
    {
        return freeIndex;
    }
    return oldestIndex;
}

// en: Clear slot for given ID (if exists).
// ja: 指定IDのスロットをクリア。
void clearSlot(const String &id)
{
    int idx = findSlotById(id);
    if (idx >= 0)
    {
        g_sessions[idx] = SessionSlot{};
    }
}

// en: Home handler issuing session ID and updating slot.
// ja: セッションIDを払い出し、スロットを更新するハンドラ。
void handleHome(EspHttpServer::Request &req, EspHttpServer::Response &res)
{
    EspHttpServer::SessionConfig cfg = buildSessionConfig();
    EspHttpServer::SessionInfo session = EspHttpServer::beginSession(req, res, cfg);

    uint64_t nowMs = millis();
    int idx = findSlotById(session.id);
    if (idx < 0)
    {
        idx = acquireSlotIndex();
        if (idx < 0)
        {
            res.sendError(500);
            return;
        }
        g_sessions[idx].used = true;
        g_sessions[idx].id = session.id;
        g_sessions[idx].hits = 0;
    }

    g_sessions[idx].hits += 1;
    g_sessions[idx].lastAccessMs = nowMs;

    const uint32_t hits = g_sessions[idx].hits;
    const int slotNo = idx;

    res.setTemplateHandler([&](const String &key, Print &out) {
        if (key == "sessionId")
        {
            out.print(session.id);
            return true;
        }
        if (key == "hits")
        {
            out.print(hits);
            return true;
        }
        if (key == "slot")
        {
            out.print(slotNo);
            return true;
        }
        if (key == "capacity")
        {
            out.print(kMaxSessions);
            return true;
        }
        return false;
    });

    res.sendText(200, "text/html", FPSTR(kHtml));
}

// en: Clear current session (cookie + slot) then redirect.
// ja: 現在のセッションを削除し、リダイレクト。
void handleClear(EspHttpServer::Request &req, EspHttpServer::Response &res)
{
    EspHttpServer::SessionConfig cfg = buildSessionConfig();
    EspHttpServer::SessionInfo session = EspHttpServer::beginSession(req, res, cfg);
    clearSlot(session.id);
    res.clearCookie(cfg.cookieName);
    res.redirect("/");
}

// en: Rotate session ID while preserving slot data; reuse slot or evict oldest.
// ja: セッションIDのみ変更し、データは新規スロットで初期化。空きが無ければ最古を再利用。
void handleRotate(EspHttpServer::Request &req, EspHttpServer::Response &res)
{
    EspHttpServer::SessionConfig cfg = buildSessionConfig();
    EspHttpServer::SessionInfo session = EspHttpServer::beginSession(req, res, cfg);
    session = EspHttpServer::rotateSession(session, res, cfg);

    const uint64_t nowMs = millis();
    int idx = findSlotById(session.id);
    if (idx < 0)
    {
        idx = acquireSlotIndex();
    }
    if (idx < 0)
    {
        res.sendError(500);
        return;
    }
    g_sessions[idx].used = true;
    g_sessions[idx].id = session.id;
    g_sessions[idx].hits = 0; // new session starts fresh
    g_sessions[idx].lastAccessMs = nowMs;

    res.redirect("/");
}

void setup()
{
    Serial.begin(115200);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
    }

    server.on("/", HTTP_GET, handleHome);
    server.on("/rotate", HTTP_POST, handleRotate);
    server.on("/clear", HTTP_POST, handleClear);
    server.begin();

    Serial.printf("Server ready: http://%s/\n", WiFi.localIP().toString().c_str());
}

void loop()
{
    delay(1);
}
