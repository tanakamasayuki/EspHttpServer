// en: Return text from a dynamic HTTP route
// ja: 動的HTTPルートから文字列を返す
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
  // en: Initialize this component after its prerequisites; inspect the returned status where provided.
  // ja: 前提となる機器を準備してから初期化し、結果を返すAPIでは成否を確認します。
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
  delay(1);
}
