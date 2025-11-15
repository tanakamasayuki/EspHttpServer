#include <WiFi.h>
#include <EspHttpServer.h>

using namespace EspHttpServer;

Server server;

constexpr const char* kSsid = "YOUR_WIFI_SSID";
constexpr const char* kPass = "YOUR_WIFI_PASS";

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(kSsid, kPass);
  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
  }

  server.begin();

  server.on("/template", HTTP_GET, [](Request& req, Response& res) {
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

    res.sendText(200, "text/html", html);
  });
}

void loop() {
}
