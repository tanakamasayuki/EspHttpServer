#include <WiFi.h>
#include <EspHttpServer.h>

// en: Header generated from assets_www by the Arduino CLI Wrapper.
// ja: Arduino CLI Wrapper で生成されたヘッダーファイルをインクルード。
#include "assets_www_embed.h"

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

  if (!server.begin())
  {
    // en: Abort if the server fails to start.
    // ja: サーバー起動に失敗したら処理を終了。
    Serial.println("Failed to start server");
    return;
  }

  Serial.printf("Server ready: http://%s/embed\n", ipStr.c_str());

  // en: Serve embedded assets directly from flash.
  // ja: フラッシュに埋め込んだアセットを配信。
  server.serveStatic("/embed",
                     assets_www_file_names,
                     assets_www_file_data,
                     assets_www_file_sizes,
                     assets_www_file_count,
                     [](const EspHttpServer::StaticInfo &info, EspHttpServer::Request &req, EspHttpServer::Response &res)
                     {
                       (void)req;
                       if (!info.exists)
                       {
                         res.redirect("/embed/index.html");
                         return;
                       }
                       res.sendStatic();
                     });
}

void loop()
{
  delay(1);
}
