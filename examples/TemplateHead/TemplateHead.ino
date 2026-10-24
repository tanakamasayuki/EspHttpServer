// en: Fill templates and insert content into HTML head
// ja: Templateを埋めてHTML headへ内容を挿入する
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
  // en: Initialize this component after its prerequisites; inspect the returned status where provided.
  // ja: 前提となる機器を準備してから初期化し、結果を返すAPIでは成否を確認します。
  server.begin();
  Serial.printf("Server ready: http://%s/template\n", ipStr.c_str());

  // en: Register the route path and HTTP method; the handler receives request-scoped objects.
  // ja: PathとHTTP Methodを登録します。Handlerのオブジェクトは要求の寿命内で扱います。
  server.on("/template", HTTP_GET, [](EspHttpServer::Request &req, EspHttpServer::Response &res)
            {
    (void)req;

    // en: Inject runtime variables into the HTML template.
    // ja: 実行時変数をテンプレートへ埋め込む。
    res.setTemplateHandler([](const String& key, Print& out) -> bool {
      if (key == "name") {
        out.print("<b>ESP32</b>");
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
  delay(1);
}
