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

  // en: Keep waiting until connected or timeout logic is added.
  // ja: 接続完了まで待機（必要に応じてタイムアウト処理を追加してください）。
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
  }

  const String ipStr = WiFi.localIP().toString();
  server.begin();
  Serial.printf("Server ready: http://%s/\n", ipStr.c_str());

  // en: Simplest handler returning plain text dynamically.
  // ja: プレーンテキストを返す最小のハンドラ例。
  server.on("/", HTTP_GET, [](EspHttpServer::Request &req, EspHttpServer::Response &res)
            {
    (void)req;
    res.sendText(200, "text/plain", "EspHttpServer ready!"); });
}

void loop()
{
  // en: Nothing to do; esp_http_server handles requests asynchronously.
  // ja: 特に処理不要。esp_http_server がバックグラウンドで処理します。
}
