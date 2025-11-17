#include <LittleFS.h>
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
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(300);
  }

  if (!LittleFS.begin())
  {
    // en: Bail out if filesystem mount fails.
    // ja: ファイルシステムがマウントできない場合は処理を中断。
    Serial.println("LittleFS mount failed");
    return;
  }

  const String ipStr = WiFi.localIP().toString();
  server.begin();
  Serial.printf("Server ready: http://%s/www/\n", ipStr.c_str());

  server.serveStatic("/www", LittleFS, "/wwwroot",
                     [](const EspHttpServer::StaticInfo &info, EspHttpServer::Request &req, EspHttpServer::Response &res)
                     {
                       (void)req;
                       if (!info.exists)
                       {
                         // en: Basic SPA fallback example.
                         // ja: SPA 用のフォールバック例。
                         res.sendFile(LittleFS, "/wwwroot/index.html");
                         return;
                       }
                       res.sendStatic();
                     });
}

void loop()
{
}
