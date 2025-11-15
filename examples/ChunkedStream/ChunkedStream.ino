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
    delay(250);
  }

  server.begin();

  // en: Demonstrate chunked responses by streaming a JSON array.
  // ja: JSON 配列をチャンク送信するデモハンドラ。
  server.on("/stream", HTTP_GET, [](Request& req, Response& res) {
    (void)req;
    res.beginChunked(200, "application/json");
    res.sendChunk("[\n");
    res.sendChunk("  {\"index\":0},\n");
    res.sendChunk("  {\"index\":1}\n");
    res.sendChunk("]\n");
    res.endChunked();
  });
}

void loop() {
}
