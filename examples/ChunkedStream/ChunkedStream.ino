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
    delay(250);
  }

  const String ipStr = WiFi.localIP().toString();
  server.begin();
  Serial.printf("Server ready: http://%s/stream\n", ipStr.c_str());

  // en: Demonstrate chunked responses by streaming a JSON array.
  // ja: JSON 配列をチャンク送信するデモハンドラ。
  server.on("/stream", HTTP_GET, [](EspHttpServer::Request &req, EspHttpServer::Response &res)
            {
    (void)req;
    res.beginChunked(200, "application/json");
    res.sendChunk("[\n");
    res.sendChunk("  {\"index\":0},\n");
    res.sendChunk("  {\"index\":1}\n");
    res.sendChunk("]\n");
    res.endChunked(); });
}

void loop()
{
}
