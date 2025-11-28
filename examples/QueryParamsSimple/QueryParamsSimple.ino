#include <WiFi.h>
#include <EspHttpServer.h>

EspHttpServer::Server server;

#if __has_include("arduino_secrets.h")
#include "arduino_secrets.h"
#else
#define WIFI_SSID "YourSSID"
#define WIFI_PASS "YourPassword"
#endif

void handleRoot(EspHttpServer::Request &req, EspHttpServer::Response &res)
{
  static const char kHtml[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Query params demo</title>
</head>
<body>
  <h1>Query parameters</h1>
  <p>例: <code>?name=esp32&lang=ja</code></p>
  <div>name = <code>{{name}}</code></div>
  <div>lang = <code>{{lang}}</code></div>
  <h2>All params</h2>
  <ul>{{{list}}}</ul>
</body>
</html>
)HTML";

  const String name = req.queryParam("name");
  const String lang = req.queryParam("lang");
  String list;
  req.forEachQueryParam([&](const String &k, const String &v) {
    list += "<li><code>" + k + "</code> = <code>" + v + "</code></li>";
    return true;
  });

  res.setTemplateHandler([&](const String &key, Print &out) {
    if (key == "name")
    {
      out.print(name.isEmpty() ? "(none)" : name);
      return true;
    }
    if (key == "lang")
    {
      out.print(lang.isEmpty() ? "(none)" : lang);
      return true;
    }
    if (key == "list")
    {
      out.print(list);
      return true;
    }
    return false;
  });

  res.sendText(200, "text/html", FPSTR(kHtml));
}

void setup()
{
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(200);
  }

  server.on("/", HTTP_GET, handleRoot);
  server.begin();
  Serial.printf("Server ready: http://%s/\n", WiFi.localIP().toString().c_str());
}

void loop()
{
  delay(1);
}
