#include <WiFi.h>
#include <EspHttpServer.h>

EspHttpServer::Server server;

#if __has_include("arduino_secrets.h")
#include "arduino_secrets.h"
#else
#define WIFI_SSID "YourSSID"
#define WIFI_PASS "YourPassword"
#endif

void renderForm(EspHttpServer::Response &res)
{
  static const char kHtml[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Form params demo</title>
</head>
<body>
  <h1>POST form (application/x-www-form-urlencoded)</h1>
  <form method="POST" action="/submit">
    <div><label>Name <input name="name" value="esp32"></label></div>
    <div><label>Lang <input name="lang" value="ja"></label></div>
    <button type="submit">Send</button>
  </form>
</body>
</html>
)HTML";
  res.sendText(200, "text/html", FPSTR(kHtml));
}

void handleSubmit(EspHttpServer::Request &req, EspHttpServer::Response &res)
{
  const String name = req.formParam("name");
  const String lang = req.formParam("lang");

  static const char kResult[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Form result</title>
</head>
<body>
  <h1>Received form</h1>
  <div>name = <code>{{name}}</code></div>
  <div>lang = <code>{{lang}}</code></div>
  <h2>All form fields</h2>
  <ul>{{{list}}}</ul>
  <p><a href='/'>Back</a></p>
</body>
</html>
)HTML";

  String list;
  req.forEachFormParam([&](const String &k, const String &v) {
    list += "<li><code>" + k + "</code> = <code>" + v + "</code></li>";
    return true;
  });

  res.setTemplateHandler([&](const String &key, Print &out) {
    if (key == "name")
    {
      out.print(name.isEmpty() ? "(none)" : name);
      return true;
    }
    if (key == "lang")
    {
      out.print(lang.isEmpty() ? "(none)" : lang);
      return true;
    }
    if (key == "list")
    {
      out.print(list);
      return true;
    }
    return false;
  });

  res.sendText(200, "text/html", FPSTR(kResult));
}

void setup()
{
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(200);
  }

  server.on("/", HTTP_GET, [](EspHttpServer::Request &req, EspHttpServer::Response &res) {
    (void)req;
    renderForm(res);
  });

  server.on("/submit", HTTP_POST, handleSubmit);
  server.begin();
  Serial.printf("Server ready: http://%s/\n", WiFi.localIP().toString().c_str());
}

void loop()
{
  delay(1);
}
