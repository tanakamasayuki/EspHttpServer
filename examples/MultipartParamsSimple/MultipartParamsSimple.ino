// en: Consume multipart form fields and uploaded content
// ja: MultipartのForm項目とアップロードを読み取る
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
  <title>Multipart demo</title>
</head>
<body>
  <h1>multipart/form-data</h1>
  <form method="POST" action="/upload" enctype="multipart/form-data">
    <div><label>Name <input name="name" value="esp32"></label></div>
    <div><label>Note <input name="note" value="hello"></label></div>
    <div><label>File <input type="file" name="file"></label></div>
    <button type="submit">Upload</button>
  </form>
</body>
</html>
)HTML";
  res.sendText(200, "text/html", FPSTR(kHtml));
}

void handleUpload(EspHttpServer::Request &req, EspHttpServer::Response &res)
{
  static const char kResult[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Multipart result</title>
</head>
<body>
  <h1>Received multipart</h1>
  <ul>{{{list}}}</ul>
  <p><a href="/">Back</a></p>
</body>
</html>
)HTML";

  String list;
  // en: Consume each scoped content stream; accepting an upload does not save it automatically.
  // ja: 各Content Streamを寿命内で消費します。受け付けても自動で保存されるわけではありません。
  req.onMultipart([&](const EspHttpServer::Request::MultipartFieldInfo &info, Stream &content) {
    list += "<li><strong>" + info.name + "</strong>";
    if (!info.filename.isEmpty())
    {
      list += " (filename=" + info.filename + ")";
    }
    if (!info.contentType.isEmpty())
    {
      list += " type=" + info.contentType;
    }
    list += " size=" + String(info.size);

    String data;
    if (info.size <= 256)
    {
      while (content.available() > 0)
      {
        data += static_cast<char>(content.read());
      }
      list += " value=<code>" + data + "</code>";
    }
    else
    {
      while (content.available() > 0)
      {
        content.read();
      }
      list += " (content skipped)";
    }
    list += "</li>";
    return true;
  });

  res.setTemplateHandler([&](const String &key, Print &out) {
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

  // en: Register the route path and HTTP method; the handler receives request-scoped objects.
  // ja: PathとHTTP Methodを登録します。Handlerのオブジェクトは要求の寿命内で扱います。
  server.on("/", HTTP_GET, [](EspHttpServer::Request &req, EspHttpServer::Response &res) {
    (void)req;
    renderForm(res);
  });
  server.on("/upload", HTTP_POST, handleUpload);
  // en: Initialize this component after its prerequisites; inspect the returned status where provided.
  // ja: 前提となる機器を準備してから初期化し、結果を返すAPIでは成否を確認します。
  server.begin();
  Serial.printf("Server ready: http://%s/\n", WiFi.localIP().toString().c_str());
}

void loop()
{
  delay(1);
}
