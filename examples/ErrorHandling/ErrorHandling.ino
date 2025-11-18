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
