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

  server.on("/", HTTP_GET, handleHome);
  server.on("/login", HTTP_POST, handleLogin);
  server.on("/logout", HTTP_POST, handleLogout);
  server.begin();

  Serial.printf("Server ready: http://%s/\n", WiFi.localIP().toString().c_str());
}

void loop()
{
  delay(1);
}
