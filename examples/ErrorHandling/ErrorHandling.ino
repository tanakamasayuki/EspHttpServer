// en: Customize error and not-found responses
// ja: ErrorとNot Foundの応答を変更する
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

  // en: Initialize this component after its prerequisites; inspect the returned status where provided.
  // ja: 前提となる機器を準備してから初期化し、結果を返すAPIでは成否を確認します。
  if (!server.begin())
  {
    Serial.println("Failed to start server");
    return;
  }

  EspHttpServer::Response::setErrorRenderer(
      [](int status, EspHttpServer::Request &req, EspHttpServer::Response &res)
      {
        String html = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>Error</title>
</head>
<body>
  <h1>ERROR {{code}}</h1>
  <p>Path: {{path}}</p>
</body>
</html>
)HTML";
        html.replace("{{code}}", String(status));
        html.replace("{{path}}", req.path());
        res.sendText(status, "text/html", html);
      });

  // en: Register the route path and HTTP method; the handler receives request-scoped objects.
  // ja: PathとHTTP Methodを登録します。Handlerのオブジェクトは要求の寿命内で扱います。
  server.on("/ok", HTTP_GET, [](EspHttpServer::Request &req, EspHttpServer::Response &res)
            { (void)req;
              res.sendText(200, "text/plain", "Everything OK"); });

  server.on("/fail", HTTP_GET, [](EspHttpServer::Request &req, EspHttpServer::Response &res)
            { (void)req;
              res.sendError(500); });

  server.onNotFound([](EspHttpServer::Request &req, EspHttpServer::Response &res)
                    {
                      (void)req;
                      res.sendError(404); });

  Serial.printf("Error handling demo: http://%s/\n", WiFi.localIP().toString().c_str());
}

void loop()
{
  delay(1);
}
