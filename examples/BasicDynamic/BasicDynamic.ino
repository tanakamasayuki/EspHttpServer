#include <WiFi.h>
#include <EspHttpServer.h>

using namespace EspHttpServer;

Server server;

// en: Replace with your Wi-Fi credentials.
// ja: Wi-Fi 設定を環境に合わせて書き換えてください。
constexpr const char* kSsid = "YOUR_WIFI_SSID";
constexpr const char* kPass = "YOUR_WIFI_PASS";

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(kSsid, kPass);

  // en: Keep waiting until connected or timeout logic is added.
  // ja: 接続完了まで待機（必要に応じてタイムアウト処理を追加してください）。
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  server.begin();

  // en: Simplest handler returning plain text dynamically.
  // ja: プレーンテキストを返す最小のハンドラ例。
  server.on("/", HTTP_GET, [](Request& req, Response& res) {
    (void)req;
    res.sendText(200, "text/plain", "EspHttpServer ready!");
  });
}

void loop() {
  // en: Nothing to do; esp_http_server handles requests asynchronously.
  // ja: 特に処理不要。esp_http_server がバックグラウンドで処理します。
}
