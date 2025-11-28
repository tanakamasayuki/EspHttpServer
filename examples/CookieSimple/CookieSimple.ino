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

// en: Simple HTML template with a visit counter placeholder.
// ja: 訪問回数を差し込むシンプルなHTMLテンプレート。
const char kHtml[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Cookie & Session demo</title>
  <style>
    body{font-family:system-ui,-apple-system,"Noto Sans JP",sans-serif;padding:24px;line-height:1.6;}
    code{background:#f1f5f9;padding:2px 6px;border-radius:6px;}
    button{padding:8px 12px;font-size:14px;}
    section{margin-bottom:16px;}
    .badge{display:inline-block;padding:4px 8px;border-radius:8px;background:#e0f2fe;color:#0369a1;font-size:12px;margin-left:8px;}
  </style>
</head>
<body>
  <h1>Cookie の超シンプル例</h1>
  <section>
    <p>このページにアクセスすると、<code>visit</code> Cookie に訪問回数を保存します。</p>
  </section>
  <section>
    <div>訪問回数 (Cookie): <code>{{visits}}</code></div>
  </section>
  <section>
    <form action="/clear" method="POST"><button type="submit">Cookie をクリア</button></form>
  </section>
</body>
</html>
)HTML";

// en: Read visit count from the Cookie (0 if missing or invalid).
// ja: Cookie に保存された訪問回数を取得する（無い/不正なら 0）。
uint32_t readVisit(const EspHttpServer::Request &req)
{
  String raw = req.cookie("visit");
  if (raw.isEmpty())
  {
    return 0;
  }
  char *end = nullptr;
  unsigned long v = strtoul(raw.c_str(), &end, 10);
  if (!end || *end != '\0')
  {
    return 0;
  }
  return static_cast<uint32_t>(v);
}

// en: Send/update the visit count cookie.
// ja: 訪問回数 Cookie を送信する。
void writeVisit(EspHttpServer::Response &res, uint32_t count)
{
  EspHttpServer::Cookie c;
  c.name = "visit";
  c.value = String(count);
  c.maxAge = 24 * 60 * 60; // 1日
  res.setCookie(c);
}

// en: Simple home: store visit count in a cookie.
// ja: シンプルなホーム: 訪問回数を Cookie に保存。
void handleHome(EspHttpServer::Request &req, EspHttpServer::Response &res)
{
  uint32_t visits = readVisit(req) + 1;
  writeVisit(res, visits);

  // en: Render HTML via template placeholders.
  // ja: テンプレートプレースホルダでHTMLをレンダリング。
  res.setTemplateHandler([&](const String &key, Print &out)
                         {
        if (key == "visits")
        {
            out.print(visits);
            return true;
        }
        return false; });

  res.sendText(200, "text/html", FPSTR(kHtml));
}

// en: Clear cookies and redirect to home.
// ja: Cookie を削除してトップへリダイレクト。
void handleClear(EspHttpServer::Request &req, EspHttpServer::Response &res)
{
  (void)req;
  res.clearCookie("visit");
  res.clearCookie("sid");
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
  server.on("/clear", HTTP_POST, handleClear);
  server.begin();

  Serial.printf("Server ready: http://%s/\n", WiFi.localIP().toString().c_str());
}

void loop()
{
  delay(1);
}
