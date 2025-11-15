#include <LittleFS.h>
#include <WiFi.h>
#include <EspHttpServer.h>

using namespace EspHttpServer;

Server server;

constexpr const char* kSsid = "YOUR_WIFI_SSID";
constexpr const char* kPass = "YOUR_WIFI_PASS";

void setup() {
  Serial.begin(115200);
  WiFi.begin(kSsid, kPass);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
  }

  if (!LittleFS.begin()) {
    // en: Bail out if filesystem mount fails.
    // ja: ファイルシステムがマウントできない場合は処理を中断。
    Serial.println("LittleFS mount failed");
    return;
  }

  server.begin();

  server.serveStatic("/www", LittleFS, "/wwwroot",
                     [](const StaticInfo& info, Request& req, Response& res) {
                       (void)req;
                       if (!info.exists) {
                         // en: Basic SPA fallback example.
                         // ja: SPA 用のフォールバック例。
                         res.sendFile(LittleFS, "/wwwroot/index.html");
                         return;
                       }
                       res.sendStatic();
                     });
}

void loop() {
}
