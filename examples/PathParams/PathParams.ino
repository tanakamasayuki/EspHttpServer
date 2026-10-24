// en: Extract values from a matched route path
// ja: 一致したルートのPathから値を取り出す
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
  Serial.printf("Param demo ready: http://%s/\n", ipStr.c_str());

  // en: Register the route path and HTTP method; the handler receives request-scoped objects.
  // ja: PathとHTTP Methodを登録します。Handlerのオブジェクトは要求の寿命内で扱います。
  server.on("/users/:userId/orders/:orderId/items/:itemId", HTTP_GET,
            [](EspHttpServer::Request &req, EspHttpServer::Response &res)
            {
              const String userId = req.pathParam("userId");
              const String orderId = req.pathParam("orderId");
              const String itemId = req.pathParam("itemId");

              String payload = "{";
              payload += "\"userId\":\"" + userId + "\",";
              payload += "\"orderId\":\"" + orderId + "\",";
              payload += "\"itemId\":\"" + itemId + "\"";
              payload += "}";

              res.sendText(200, "application/json", payload);
            });

  server.on("/files/:type/*path", HTTP_GET,
            [](EspHttpServer::Request &req, EspHttpServer::Response &res)
            {
              const String rest = req.pathParam("path");
              String type = req.pathParam("type");
              String message = "Requested type: " + type + "\n" +
                               "Requested path: " + rest;
              res.sendText(200, "text/plain", message);
            });
}

void loop()
{
  delay(1);
}
