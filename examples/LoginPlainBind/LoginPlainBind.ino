// en: Demonstrate login state and HTML data binding
// ja: ログイン状態とHTMLへのデータ反映を試す
#include <WiFi.h>
#include <EspHttpServer.h>
#include <EspHelperJsonGenerator.h>
#include <AssocTree.h>
#include "assets_embed.h"

EspHttpServer::Server server;
assoc_tree::AssocTree<4096> sessionStore;

static const char *kValidUser = "demo";
static const char *kValidPass = "password";

#if __has_include("arduino_secrets.h")
#include "arduino_secrets.h"
#else
#define WIFI_SSID "YourSSID"
#define WIFI_PASS "YourPassword"
#endif

struct SessionData
{
  bool loggedIn = false;
  String user;
};

EspHttpServer::SessionConfig buildSessionConfig()
{
  EspHttpServer::SessionConfig cfg;
  cfg.cookieName = "sid";
  cfg.maxAgeSeconds = 7 * 24 * 60 * 60;
  cfg.path = "/";
  cfg.httpOnly = true;
  cfg.sameSite = EspHttpServer::Cookie::SameSite::Lax;
  return cfg;
}

SessionData loadSession(const String &sid)
{
  SessionData data;
  auto node = sessionStore["sessions"][sid.c_str()];
  if (node.isObject())
  {
    data.loggedIn = node["loggedIn"].as(false);
    const char *u = node["user"].asCString("");
    if (u)
    {
      data.user = u;
    }
  }
  return data;
}

void saveSession(const String &sid, const SessionData &data)
{
  auto node = sessionStore["sessions"][sid.c_str()];
  node["loggedIn"] = data.loggedIn;
  node["user"] = data.user.c_str();
  node["updated"] = static_cast<int32_t>(millis());
}

void clearSession(const String &sid)
{
  auto node = sessionStore["sessions"][sid.c_str()];
  node.unset();
  // en: Compact the tree pool and reacquire all attached references afterwards.
  // ja: Treeの領域を詰め直すので、あとで参照を取り直します。
  sessionStore.gc();
}

String buildHeadJson(const SessionData &session, const String &message)
{
  static char buffer[512];
  EspHelper::JsonGenerator gen(buffer, sizeof(buffer));
  gen.startObject();
  gen.setString("title", "PlainBind login demo");

  gen.pushObject("credentials");
  gen.setString("id", kValidUser);
  gen.setString("pass", kValidPass);
  gen.popObject();

  gen.pushObject("session");
  gen.setBool("loggedIn", session.loggedIn);
  gen.setString("user", session.user);
  gen.popObject();

  gen.setString("message", message);
  gen.endObject();
  // en: Finalize the generated structure and check capacity/error status before using the output.
  // ja: 生成中の構造を完了し、容量／Errorを確認してから出力を使います。
  gen.finish();
  return String(buffer);
}

void setHeadData(EspHttpServer::Response &res, const SessionData &session, const String &message)
{
  String json = buildHeadJson(session, message);
  String head;
  head.reserve(json.length() + 160);
  head += "<script id=\"plainbind-data\" type=\"application/json\">";
  head += json;
  head += "</script>";
  res.setHeadInjection(head);
}

void renderHome(EspHttpServer::Request &req, EspHttpServer::Response &res, const SessionData &session, const String &message)
{
  (void)req;
  setHeadData(res, session, message);
  // en: Submit the supplied data and check the API-specific result; acceptance is different from later processing.
  // ja: データを渡して対象APIの結果を確認します。受理されたことと後の処理完了は別です。
  res.send(200, "text/html", assets_index_html, assets_index_html_len);
}

void handleHome(EspHttpServer::Request &req, EspHttpServer::Response &res)
{
  EspHttpServer::SessionConfig cfg = buildSessionConfig();
  EspHttpServer::SessionInfo info = EspHttpServer::beginSession(req, res, cfg);
  SessionData session = loadSession(info.id);
  renderHome(req, res, session, "");
}

void handleLogin(EspHttpServer::Request &req, EspHttpServer::Response &res)
{
  EspHttpServer::SessionConfig cfg = buildSessionConfig();
  EspHttpServer::SessionInfo info = EspHttpServer::beginSession(req, res, cfg);
  String id = req.formParam("id");
  String pass = req.formParam("pass");
  SessionData session;
  String message;
  if (id == kValidUser && pass == kValidPass)
  {
    session.loggedIn = true;
    session.user = id;
    saveSession(info.id, session);
    res.redirect("/");
    return;
  }
  else
  {
    session = loadSession(info.id);
    message = F("ID またはパスワードが違います");
  }
  renderHome(req, res, session, message);
}

void handleLogout(EspHttpServer::Request &req, EspHttpServer::Response &res)
{
  EspHttpServer::SessionConfig cfg = buildSessionConfig();
  EspHttpServer::SessionInfo info = EspHttpServer::beginSession(req, res, cfg);
  clearSession(info.id);
  res.clearCookie(cfg.cookieName);
  res.redirect("/");
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
  server.on("/", HTTP_GET, handleHome);
  server.on("/login", HTTP_POST, handleLogin);
  server.on("/logout", HTTP_POST, handleLogout);
  // en: Initialize this component after its prerequisites; inspect the returned status where provided.
  // ja: 前提となる機器を準備してから初期化し、結果を返すAPIでは成否を確認します。
  server.begin();

  Serial.printf("Server ready: http://%s/\n", WiFi.localIP().toString().c_str());
}

void loop()
{
  delay(1);
}
