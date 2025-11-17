#include <WiFi.h>
#include <EspHttpServer.h>

EspHttpServer::Server server;

#if __has_include("arduino_secrets.h")
#include "arduino_secrets.h"
#else
#define WIFI_SSID "YourSSID"     // Enter your Wi-Fi SSID here / Wi-FiのSSIDを入力
#define WIFI_PASS "YourPassword" // Enter your Wi-Fi password here / Wi-Fiのパスワードを入力
#endif

void setup()
{
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(200);
  }

  const String ipStr = WiFi.localIP().toString();
  server.begin();
  Serial.printf("Server ready: http://%s/template\n", ipStr.c_str());

  server.on("/template", HTTP_GET, [](EspHttpServer::Request &req, EspHttpServer::Response &res)
            {
    (void)req;

    // en: Inject runtime variables into the HTML template.
    // ja: 実行時変数をテンプレートへ埋め込む。
    res.setTemplateHandler([](const String& key, Print& out) -> bool {
      if (key == "name") {
        out.print("ESP32");
        return true;
      }
      if (key == "version") {
        out.print("0.0.1");
        return true;
      }
      return false;
    });

    // en: Insert extra HTML immediately after <head>.
    // ja: `<head>` タグ直後に追記する HTML。
    res.setHeadInjection("<meta name='injected' content='true'>");

    const String html = R"HTML(
<!doctype html>
<html>
<head>
  <title>{{name}} demo</title>
</head>
<body>
  <h1>{{{name}}}</h1>
  <p>Version: {{version}}</p>
</body>
</html>
)HTML";

    res.sendText(200, "text/html", html); });
}

void loop()
{
}
