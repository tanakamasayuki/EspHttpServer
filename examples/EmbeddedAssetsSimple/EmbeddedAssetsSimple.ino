#include <WiFi.h>
#include <EspHttpServer.h>

// en: Header generated from assets_www by the Arduino CLI Wrapper.
// ja: Arduino CLI Wrapper で生成されたヘッダーファイルをインクルード。
#include "assets_www_embed.h"

using namespace EspHttpServer;

Server server;

constexpr const char* kSsid = "YOUR_WIFI_SSID";
constexpr const char* kPass = "YOUR_WIFI_PASS";

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(kSsid, kPass);
  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
  }

  if (!server.begin()) {
    Serial.println("Failed to start server");
    return;
  }

  // en: Serve embedded assets directly from flash.
  // ja: フラッシュに埋め込んだアセットを配信。
  server.serveStatic("/embed",
                     assets_www_file_names,
                     assets_www_file_data,
                     assets_www_file_sizes,
                     assets_www_file_count,
                     [](const StaticInfo& info, Request& req, Response& res) {
                       (void)req;
                       if (!info.exists) {
                         res.redirect("/embed/index.html");
                         return;
                       }
                       res.sendStatic();
                     });
}

void loop() {
}
