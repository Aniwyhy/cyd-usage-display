// Split-screen usage display for the 4" CYD (ESP32-32E, ST7796 480x320).
// Left half: Claude plan limits. Right half: ChatGPT plan (Codex) limits.
// Each side has its own login, stored only on the board.
//
//  - Claude: OAuth (PKCE) through the web page the board serves, scope user:profile
//    (can read usage, can't send prompts). Same endpoint as Claude Code's /usage.
//  - ChatGPT: device-code login (like `codex login --device-auth`): the board shows a
//    code, you enter it at auth.openai.com/codex/device. Same endpoint as Codex's /status.
//
// First boot: the board opens a WiFi hotspot; scan the QR on screen to set WiFi.
// BOOT button: tap = refresh now (or new ChatGPT code), hold 5 s = forget everything.
#define LGFX_USE_V1
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>
#include <time.h>
#include "certs.h"

// ---------------------------------------------------------------- config
static const char *TZ_INFO = "CET-1CEST,M3.5.0,M10.5.0/3";  // Europe/Madrid
static const char *HOSTNAME = "claude-usage";
static const uint32_t POLL_MS = 5UL * 60 * 1000;

// Claude
static const char *CL_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
static const char *CL_AUTH_URL = "https://claude.com/cai/oauth/authorize";
static const char *CL_TOKEN_URL = "https://platform.claude.com/v1/oauth/token";
static const char *CL_REDIRECT = "https://platform.claude.com/oauth/code/callback";
static const char *CL_SCOPE = "user:profile";
static const char *CL_USAGE_URL = "https://api.anthropic.com/api/oauth/usage";
static const char *CL_PROFILE_URL = "https://api.anthropic.com/api/oauth/profile";

// ChatGPT (Codex)
static const char *OA_CLIENT_ID = "app_EMoamEEZ73f0CkXaXp7hrann";
static const char *OA_USERCODE_URL = "https://auth.openai.com/api/accounts/deviceauth/usercode";
static const char *OA_POLL_URL = "https://auth.openai.com/api/accounts/deviceauth/token";
static const char *OA_TOKEN_URL = "https://auth.openai.com/oauth/token";
static const char *OA_REDIRECT = "https://auth.openai.com/deviceauth/callback";
static const char *OA_VERIFY_URL = "https://auth.openai.com/codex/device";
static const char *OA_USAGE_URL = "https://chatgpt.com/backend-api/wham/usage";

// ---------------------------------------------------------------- display
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;

 public:
  LGFX() {
    auto b = _bus.config();
    b.spi_host = HSPI_HOST;
    b.spi_mode = 0;
    b.freq_write = 40000000;
    b.freq_read = 16000000;
    b.pin_sclk = 14;
    b.pin_mosi = 13;
    b.pin_miso = 12;
    b.pin_dc = 2;
    _bus.config(b);
    _panel.setBus(&_bus);
    auto p = _panel.config();
    p.pin_cs = 15;
    p.pin_rst = -1;
    p.panel_width = 320;
    p.panel_height = 480;
    p.readable = true;
    p.bus_shared = true;
    _panel.config(p);
    auto l = _light.config();
    l.pin_bl = 27;
    l.freq = 44100;
    l.pwm_channel = 7;
    _light.config(l);
    _panel.setLight(&_light);
    setPanel(&_panel);
  }
};
static LGFX tft;
static LGFX_Sprite gauge(&tft);

struct RGB { uint8_t r, g, b; };
static const RGB C_BG = {0x17, 0x16, 0x15};
static const RGB C_CARD = {0x24, 0x23, 0x21};
static const RGB C_TRACK = {0x36, 0x34, 0x31};
static const RGB C_FG = {0xF5, 0xF4, 0xEE};
static const RGB C_DIM = {0x9C, 0x9A, 0x92};
static const RGB C_CLAUDE = {0xD9, 0x77, 0x57};
static const RGB C_GPT = {0x19, 0xC3, 0x7D};
static const RGB C_AMBER = {0xE8, 0xA2, 0x3A};
static const RGB C_RED = {0xE5, 0x48, 0x4D};
static const RGB C_GREEN = {0x6B, 0xC4, 0x7E};
static inline uint16_t c565(RGB c) { return lgfx::color565(c.r, c.g, c.b); }
static inline RGB mix(RGB a, RGB b, float t) {
  return {uint8_t(a.r + (b.r - a.r) * t), uint8_t(a.g + (b.g - a.g) * t), uint8_t(a.b + (b.b - a.b) * t)};
}

// ---------------------------------------------------------------- state
struct Win { String label; float pct; time_t reset; };

struct Account {
  const char *name;
  const char *pfx;  // NVS key prefix
  RGB brand;
  String refreshTok, accessTok, plan, acctId;
  uint32_t accessExp = 0;
  Win wins[2];  // [0] short window (5-hour), [1] weekly
  int nWins = 0;
  String extra;  // other limits, one line
  float shown[2] = {0, 0};
  bool haveData = false;
  String lastError;
  uint32_t lastOk = 0, nextFetchAt = 0;
  Account(const char *n, const char *p, RGB b) : name(n), pfx(p), brand(b) {}
  bool signedIn() const { return refreshTok.length() > 0; }
};
static Account acc[2] = {Account("Claude", "", C_CLAUDE), Account("ChatGPT", "o_", C_GPT)};
enum { CL = 0, OA = 1 };

// ChatGPT device-code flow
struct DeviceFlow {
  bool active = false, expired = false;
  String authId, userCode, error;
  uint32_t interval = 5, nextPoll = 0, expiresAt = 0;
} dev;

// Claude PKCE (kept until used so reloading the page doesn't break the link)
static String pkceVerifier, pkceState;

enum Mode { M_BOOT, M_AP, M_RUN };
static Mode mode = M_BOOT;
static Preferences prefs;
static WebServer server(80);
static DNSServer dns;
static String apPass, wifiSsid, wifiPass, connectError;

// ---------------------------------------------------------------- helpers
static time_t nowT() { return time(nullptr); }
static bool timeValid() { return nowT() > 1700000000; }

static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}
static time_t parseIso(const char *s) {
  if (!s || strlen(s) < 19) return 0;
  int Y, M, D, h, m, sec;
  if (sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
  int64_t t = daysFromCivil(Y, M, D) * 86400 + h * 3600 + m * 60 + sec;
  const char *z = s + 19;
  while (*z == '.' || isdigit((unsigned char)*z)) z++;
  if (*z == '+' || *z == '-') {
    int oh = 0, om = 0;
    sscanf(z + 1, "%d:%d", &oh, &om);
    int off = oh * 3600 + om * 60;
    t -= (*z == '+') ? off : -off;
  }
  return (time_t)t;
}

static String b64url(const uint8_t *data, size_t len) {
  unsigned char out[96];
  size_t olen = 0;
  mbedtls_base64_encode(out, sizeof out, &olen, data, len);
  String s;
  for (size_t i = 0; i < olen; i++) {
    char c = out[i];
    if (c == '+') c = '-';
    else if (c == '/') c = '_';
    else if (c == '=') continue;
    s += c;
  }
  return s;
}
static String randomToken(size_t bytes) {
  uint8_t buf[48];
  esp_fill_random(buf, bytes);
  return b64url(buf, bytes);
}
static String pkceChallenge(const String &verifier) {
  uint8_t hash[32];
  mbedtls_sha256((const unsigned char *)verifier.c_str(), verifier.length(), hash, 0);
  return b64url(hash, 32);
}
static String urlEncode(const String &s) {
  String o;
  const char *hex = "0123456789ABCDEF";
  for (char c : s) {
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') o += c;
    else { o += '%'; o += hex[(c >> 4) & 15]; o += hex[c & 15]; }
  }
  return o;
}
static String htmlEsc(const String &s) {
  String o;
  for (char c : s) {
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;";
    else if (c == '\'') o += "&#39;";
    else o += c;
  }
  return o;
}
// Decode the payload (middle part) of a JWT into a JSON document.
static bool jwtClaims(const String &jwt, JsonDocument &out) {
  int a = jwt.indexOf('.'), b = jwt.indexOf('.', a + 1);
  if (a < 0 || b < 0) return false;
  String p = jwt.substring(a + 1, b);
  p.replace('-', '+');
  p.replace('_', '/');
  while (p.length() % 4) p += '=';
  size_t cap = p.length() * 3 / 4 + 4, olen = 0;
  std::unique_ptr<uint8_t[]> buf(new uint8_t[cap]);
  if (mbedtls_base64_decode(buf.get(), cap, &olen, (const unsigned char *)p.c_str(), p.length())) return false;
  return !deserializeJson(out, (const char *)buf.get(), olen);
}

static String fmtDur(long secs) {
  if (secs < 0) secs = 0;
  long d = secs / 86400, h = (secs % 86400) / 3600, m = (secs % 3600) / 60;
  char b[24];
  if (d) snprintf(b, sizeof b, "%ldd %ldh", d, h);
  else if (h) snprintf(b, sizeof b, "%ldh %02ldm", h, m);
  else snprintf(b, sizeof b, "%ldm", m < 1 ? 1 : m);
  return b;
}
static String fmtWhen(time_t t) {
  struct tm lt, ln;
  time_t n = nowT();
  localtime_r(&t, &lt);
  localtime_r(&n, &ln);
  char b[24];
  if (lt.tm_yday == ln.tm_yday && lt.tm_year == ln.tm_year) strftime(b, sizeof b, "today %H:%M", &lt);
  else strftime(b, sizeof b, "%a %H:%M", &lt);
  return b;
}
static String titleCase(String s) {
  s.replace("_", " ");
  if (s.length()) s[0] = toupper(s[0]);
  return s;
}

// ---------------------------------------------------------------- network
struct Hdr { const char *k; String v; };
static int https(const char *method, const char *url, const String &body, const char *ctype,
                 std::initializer_list<Hdr> hdrs, String &resp) {
  WiFiClientSecure client;
  client.setCACert(ROOT_CAS);
  client.setTimeout(15);
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, url)) return -100;
  http.addHeader("Accept", "application/json");
  for (auto &h : hdrs) http.addHeader(h.k, h.v);
  int code;
  if (!strcmp(method, "POST")) {
    http.addHeader("Content-Type", ctype ? ctype : "application/json");
    code = http.POST(body);
  } else {
    code = http.GET();
  }
  resp = code > 0 ? http.getString() : String(http.errorToString(code));
  http.end();
  return code;
}

// ----- Claude
static void saveTokens(Account &a) {
  String p = a.pfx;
  prefs.putString((p + "rtok").c_str(), a.refreshTok);
  prefs.putString((p + "atok").c_str(), a.accessTok);
  prefs.putUInt((p + "aexp").c_str(), a.accessExp);
  prefs.putString((p + "plan").c_str(), a.plan);
  prefs.putString((p + "acct").c_str(), a.acctId);
}
static void signOut(Account &a) {
  a.refreshTok = a.accessTok = a.plan = a.acctId = a.extra = "";
  a.accessExp = 0;
  a.nWins = 0;
  a.haveData = false;
  a.shown[0] = a.shown[1] = 0;
  String p = a.pfx;
  for (const char *k : {"rtok", "atok", "aexp", "plan", "acct"}) prefs.remove((p + k).c_str());
}

static bool clStoreTokens(const String &resp) {
  Account &a = acc[CL];
  JsonDocument d;
  if (deserializeJson(d, resp) || !d["access_token"].is<const char *>()) return false;
  a.accessTok = d["access_token"].as<const char *>();
  if (d["refresh_token"].is<const char *>()) a.refreshTok = d["refresh_token"].as<const char *>();
  uint32_t exp = d["expires_in"] | 3600;
  a.accessExp = timeValid() ? nowT() + exp : 0;
  saveTokens(a);
  Serial.printf("[claude] token ok, scope=%s\n", (const char *)(d["scope"] | "?"));
  return true;
}

static bool clRefresh() {
  Account &a = acc[CL];
  JsonDocument d;
  d["grant_type"] = "refresh_token";
  d["refresh_token"] = a.refreshTok;
  d["client_id"] = CL_CLIENT_ID;
  d["scope"] = CL_SCOPE;
  String body, resp;
  serializeJson(d, body);
  int code = https("POST", CL_TOKEN_URL, body, nullptr, {{"User-Agent", "claude-usage-cyd/1.0"}}, resp);
  Serial.printf("[claude] refresh -> %d\n", code);
  if (code == 200 && clStoreTokens(resp)) return true;
  if (code == 400 || code == 401) { signOut(a); a.lastError = "Signed out"; }
  else a.lastError = "Refresh failed (" + String(code) + ")";
  return false;
}

static int clGet(const char *url, String &resp) {
  return https("GET", url, "", nullptr,
               {{"Authorization", "Bearer " + acc[CL].accessTok}, {"anthropic-beta", "oauth-2025-04-20"},
                {"User-Agent", "claude-usage-cyd/1.0"}},
               resp);
}

static void clFetchPlan() {
  String resp;
  if (clGet(CL_PROFILE_URL, resp) != 200) return;
  JsonDocument d;
  if (deserializeJson(d, resp)) return;
  String t = d["organization"]["organization_type"] | "";
  String &plan = acc[CL].plan;
  if (t.indexOf("max") >= 0) plan = "Max";
  else if (t.indexOf("pro") >= 0) plan = "Pro";
  else if (t.indexOf("team") >= 0) plan = "Team";
  else if (t.indexOf("enterprise") >= 0) plan = "Enterprise";
  saveTokens(acc[CL]);
}

static bool clFetch() {
  Account &a = acc[CL];
  if (!a.accessTok.length() || (a.accessExp && timeValid() && nowT() > (time_t)a.accessExp - 300))
    if (!clRefresh()) return false;
  if (!a.plan.length()) clFetchPlan();
  String resp;
  int code = clGet(CL_USAGE_URL, resp);
  if (code == 401) {
    if (!clRefresh()) return false;
    code = clGet(CL_USAGE_URL, resp);
  }
  Serial.printf("[claude] usage %d\n", code);
  if (code == 429) { a.lastError = "Rate limited"; a.nextFetchAt = millis() + 10 * 60 * 1000UL; return false; }
  if (code != 200) { a.lastError = "Usage failed (" + String(code) + ")"; return false; }
  JsonDocument d;
  if (deserializeJson(d, resp)) { a.lastError = "Bad response"; return false; }
  a.nWins = 0;
  a.extra = "";
  const char *keys[] = {"five_hour", "seven_day"};
  const char *labels[] = {"5-hour", "Weekly"};
  for (int i = 0; i < 2; i++) {
    JsonVariant v = d[keys[i]];
    if (v.is<JsonObject>() && !v["utilization"].isNull())
      a.wins[a.nWins++] = {labels[i], v["utilization"].as<float>(), parseIso(v["resets_at"] | "")};
  }
  for (JsonPair p : d.as<JsonObject>()) {
    String k = p.key().c_str();
    JsonVariant v = p.value();
    if (k == "seven_day" || !k.startsWith("seven_day_") || !v.is<JsonObject>() || v["utilization"].isNull()) continue;
    k.replace("seven_day_", "");
    if (a.extra.length()) a.extra += "  ";
    a.extra += titleCase(k) + " " + String((int)roundf(v["utilization"].as<float>())) + "%";
  }
  return a.nWins > 0;
}

// ----- ChatGPT
static bool oaStoreTokens(const String &resp) {
  Account &a = acc[OA];
  JsonDocument d;
  if (deserializeJson(d, resp) || !d["access_token"].is<const char *>()) return false;
  a.accessTok = d["access_token"].as<const char *>();
  if (d["refresh_token"].is<const char *>()) a.refreshTok = d["refresh_token"].as<const char *>();
  JsonDocument c;
  if (jwtClaims(a.accessTok, c)) a.accessExp = c["exp"] | 0;
  if (d["id_token"].is<const char *>()) {
    JsonDocument idc;
    if (jwtClaims(d["id_token"].as<String>(), idc)) {
      JsonVariant auth = idc["https://api.openai.com/auth"];
      if (auth["chatgpt_account_id"].is<const char *>()) a.acctId = auth["chatgpt_account_id"].as<const char *>();
      if (auth["chatgpt_plan_type"].is<const char *>()) a.plan = titleCase(auth["chatgpt_plan_type"].as<String>());
    }
  }
  saveTokens(a);
  Serial.printf("[chatgpt] token ok, account id %s, plan %s\n", a.acctId.length() ? "found" : "MISSING", a.plan.c_str());
  return true;
}

static bool oaRefresh() {
  Account &a = acc[OA];
  JsonDocument d;
  d["client_id"] = OA_CLIENT_ID;
  d["grant_type"] = "refresh_token";
  d["refresh_token"] = a.refreshTok;
  d["scope"] = "openid profile email";
  String body, resp;
  serializeJson(d, body);
  int code = https("POST", OA_TOKEN_URL, body, nullptr, {{"User-Agent", "codex-cli"}}, resp);
  Serial.printf("[chatgpt] refresh -> %d\n", code);
  if (code == 200 && oaStoreTokens(resp)) return true;
  Serial.println(resp.substring(0, 200));
  if (code == 400 || code == 401) { signOut(a); a.lastError = "Signed out"; }
  else a.lastError = "Refresh failed (" + String(code) + ")";
  return false;
}

static void startDeviceFlow() {
  dev = DeviceFlow();
  String resp;
  int code = https("POST", OA_USERCODE_URL, String("{\"client_id\":\"") + OA_CLIENT_ID + "\"}", nullptr,
                   {{"User-Agent", "codex-cli"}}, resp);
  Serial.printf("[chatgpt] usercode -> %d\n", code);
  JsonDocument d;
  if (code != 200 || deserializeJson(d, resp)) {
    dev.error = code == 404 ? "Device login is turned off for this account" : "Couldn't get a code (" + String(code) + ")";
    return;
  }
  dev.authId = d["device_auth_id"] | "";
  dev.userCode = d["user_code"] | (d["usercode"] | "");
  dev.interval = max(3L, d["interval"].as<String>().toInt());
  if (!dev.interval) dev.interval = 5;
  dev.active = dev.authId.length() && dev.userCode.length();
  dev.expiresAt = millis() + 15 * 60 * 1000UL;
  dev.nextPoll = millis() + dev.interval * 1000;
}

// returns true when signed in
static bool pollDeviceFlow() {
  if (!dev.active || (long)(millis() - dev.nextPoll) < 0) return false;
  dev.nextPoll = millis() + dev.interval * 1000;
  if ((long)(millis() - dev.expiresAt) > 0) {
    dev.active = false;
    dev.expired = true;
    return false;
  }
  JsonDocument q;
  q["device_auth_id"] = dev.authId;
  q["user_code"] = dev.userCode;
  String body, resp;
  serializeJson(q, body);
  int code = https("POST", OA_POLL_URL, body, nullptr, {{"User-Agent", "codex-cli"}}, resp);
  if (code == 403 || code == 404) return false;  // still waiting
  Serial.printf("[chatgpt] poll -> %d\n", code);
  JsonDocument d;
  if (code != 200 || deserializeJson(d, resp)) {
    dev.active = false;
    dev.error = "Sign-in failed (" + String(code) + ")";
    return false;
  }
  String form = "grant_type=authorization_code&code=" + urlEncode(d["authorization_code"] | "") +
                "&redirect_uri=" + urlEncode(OA_REDIRECT) + "&client_id=" + OA_CLIENT_ID +
                "&code_verifier=" + urlEncode(d["code_verifier"] | "");
  code = https("POST", OA_TOKEN_URL, form, "application/x-www-form-urlencoded", {{"User-Agent", "codex-cli"}}, resp);
  Serial.printf("[chatgpt] exchange -> %d\n", code);
  dev.active = false;
  if (code != 200 || !oaStoreTokens(resp)) {
    dev.error = "Sign-in failed (" + String(code) + ")";
    return false;
  }
  return true;
}

static int oaGetUsage(String &resp) {
  Account &a = acc[OA];
  return https("GET", OA_USAGE_URL, "", nullptr,
               {{"Authorization", "Bearer " + a.accessTok}, {"chatgpt-account-id", a.acctId}, {"User-Agent", "codex-cli"}},
               resp);
}

static String windowLabel(long secs) {
  if (secs >= 6 * 86400) return "Weekly";
  if (secs >= 86400) return String(secs / 86400) + "-day";
  if (secs > 0) return String((secs + 1800) / 3600) + "-hour";
  return "Limit";
}

static bool oaFetch() {
  Account &a = acc[OA];
  if (!a.accessTok.length() || (a.accessExp && timeValid() && nowT() > (time_t)a.accessExp - 300))
    if (!oaRefresh()) return false;
  String resp;
  int code = oaGetUsage(resp);
  if (code == 401 || code == 403) {
    if (!oaRefresh()) return false;
    code = oaGetUsage(resp);
  }
  Serial.printf("[chatgpt] usage %d %s\n", code, resp.substring(0, 300).c_str());
  if (code == 429) { a.lastError = "Rate limited"; a.nextFetchAt = millis() + 10 * 60 * 1000UL; return false; }
  if (code != 200) { a.lastError = "Usage failed (" + String(code) + ")"; return false; }
  JsonDocument d;
  if (deserializeJson(d, resp)) { a.lastError = "Bad response"; return false; }
  if (d["plan_type"].is<const char *>()) {
    String p = titleCase(d["plan_type"].as<String>());
    if (p != a.plan) { a.plan = p; saveTokens(a); }
  }
  a.nWins = 0;
  a.extra = "";
  for (const char *k : {"primary_window", "secondary_window"}) {
    JsonVariant w = d["rate_limit"][k];
    if (!w.is<JsonObject>()) continue;
    time_t reset = w["reset_at"] | 0;
    if (!reset && timeValid()) reset = nowT() + (long)(w["reset_after_seconds"] | 0);
    a.wins[a.nWins++] = {windowLabel(w["limit_window_seconds"] | 0), w["used_percent"] | 0.0f, reset};
  }
  if (d["credits"]["has_credits"] | false) {
    a.extra = "Credits " + String((const char *)(d["credits"]["balance"] | "?"));
  }
  if (!a.nWins) { a.lastError = "No limits for this plan"; return false; }
  return true;
}

// ---------------------------------------------------------------- drawing
static const int TOP_H = 34, PY = 40, PW = 228, PH = 274;
static int panelX(int i) { return 8 + i * (PW + 8); }

static void drawClaudeMark(int cx, int cy, int r, RGB col, uint16_t bg) {
  for (int i = 0; i < 12; i++) {
    float a = i * PI / 6 + 0.13f;
    float len = (i % 3 == 0) ? r : r * 0.78f;
    tft.drawWedgeLine(cx, cy, cx + len * cosf(a), cy + len * sinf(a), 2.0f, 0.9f, c565(col));
  }
  tft.fillCircle(cx, cy, 3, c565(col));
}
static void drawGptMark(int cx, int cy, int r, RGB col, uint16_t bg) {  // simple hex rosette
  for (int i = 0; i < 6; i++) {
    float a = i * PI / 3;
    tft.drawWideLine(cx + r * 0.45f * cosf(a), cy + r * 0.45f * sinf(a), cx + r * cosf(a + 0.9f),
                     cy + r * sinf(a + 0.9f), 2.6f, c565(col));
  }
  tft.drawCircle(cx, cy, (int)(r * 0.45f), c565(col));
}

static void drawWifiIcon(int x, int y) {
  int rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -200;
  int bars = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : rssi > -90 ? 1 : 0;
  for (int i = 0; i < 4; i++) {
    int h = 4 + i * 3;
    tft.fillRoundRect(x + i * 5, y - h, 3, h, 1, c565(i < bars ? C_FG : C_TRACK));
  }
}

static void drawTopBar() {
  tft.fillRect(0, 0, 480, TOP_H + 2, c565(C_BG));
  if (timeValid()) {
    time_t n = nowT();
    struct tm lt;
    localtime_r(&n, &lt);
    char t[8], d[24];
    strftime(t, sizeof t, "%H:%M", &lt);
    strftime(d, sizeof d, "%a %d %b", &lt);
    tft.setFont(&fonts::FreeSansBold12pt7b);
    tft.setTextDatum(middle_center);
    tft.setTextColor(c565(C_FG));
    tft.drawString(t, 240, 19);
    tft.setFont(&fonts::FreeSans9pt7b);
    tft.setTextColor(c565(C_DIM));
    tft.setTextDatum(middle_left);
    tft.drawString(d, 14, 19);
  }
  drawWifiIcon(448, 26);
}

// Two concentric anti-aliased 270-degree rings: outer = 5-hour, inner = weekly.
static const int G = 172;
static void ring(float rm, float hw, float pct, RGB col, bool track) {
  const float cx = (G - 1) / 2.0f, cy = cx, a0 = 135, sweep = 270;
  const float a1 = a0 + sweep * constrain(pct, 0, 100) / 100.0f;
  auto capDist = [&](float px, float py, float deg) {
    float r = deg * DEG_TO_RAD;
    float dx = px - (cx + rm * cosf(r)), dy = py - (cy + rm * sinf(r));
    return sqrtf(dx * dx + dy * dy);
  };
  RGB dark = mix(col, C_CARD, 0.4f);
  int lo = max(0, (int)(cy - rm - hw - 2)), hi = min(G - 1, (int)(cy + rm + hw + 2));
  for (int y = lo; y <= hi; y++) {
    float dy = y - cy;
    for (int x = lo; x <= hi; x++) {
      float dx = x - cx, r = sqrtf(dx * dx + dy * dy), band = fabsf(r - rm);
      if (band > hw + 1.5f) continue;
      float a = atan2f(dy, dx) * RAD_TO_DEG;
      if (a < 0) a += 360;
      if (a < a0) a += 360;
      float dT = (a <= a0 + sweep) ? band : fminf(capDist(x, y, a0), capDist(x, y, a0 + sweep));
      float cT = constrain(hw + 0.5f - dT, 0.0f, 1.0f);
      if (cT <= 0) continue;
      RGB px = track ? mix(C_CARD, C_TRACK, cT) : C_CARD;
      if (pct > 0.3f) {
        float dF = (a <= a1) ? band : fminf(capDist(x, y, a0), capDist(x, y, a1));
        float cF = constrain(hw + 0.5f - dF, 0.0f, 1.0f);
        if (cF > 0) px = mix(px, mix(dark, col, constrain((a - a0) / fmaxf(a1 - a0, 1.0f), 0.0f, 1.0f)), cF);
      }
      gauge.drawPixel(x, y, c565(px));
    }
  }
}

static RGB levelColor(RGB brand, float p) { return p >= 90 ? C_RED : p >= 75 ? C_AMBER : brand; }
static RGB innerColor(RGB brand, float p) { return mix(levelColor(brand, p), C_FG, 0.35f); }

static void drawGauge(int i) {
  Account &a = acc[i];
  float p0 = a.shown[0], p1 = a.shown[1];
  gauge.fillScreen(c565(C_CARD));
  ring(78, 9, p0, levelColor(a.brand, a.nWins > 0 ? a.wins[0].pct : p0), true);
  if (a.nWins > 1) ring(57, 6, p1, innerColor(a.brand, a.wins[1].pct), true);
  const int cx = G / 2;
  char num[8];
  snprintf(num, sizeof num, "%d", (int)roundf(p0));
  gauge.setFont(&fonts::DejaVu40);
  int wn = gauge.textWidth(num);
  gauge.setFont(&fonts::DejaVu18);
  int wp = gauge.textWidth("%");
  int x0 = cx - (wn + wp + 2) / 2, base = cx + 12;
  gauge.setTextDatum(baseline_left);
  gauge.setFont(&fonts::DejaVu40);
  gauge.setTextColor(c565(C_FG));
  gauge.drawString(num, x0, base);
  gauge.setFont(&fonts::DejaVu18);
  gauge.setTextColor(c565(C_DIM));
  gauge.drawString("%", x0 + wn + 2, base - 16);
  gauge.setFont(&fonts::FreeSansBold9pt7b);
  gauge.setTextDatum(middle_center);
  gauge.drawString(a.nWins ? a.wins[0].label : "", cx, cx + 28);
  gauge.pushSprite(panelX(i) + (PW - G) / 2, PY + 40);
}

static void drawPanelHeader(int i) {
  Account &a = acc[i];
  int x = panelX(i);
  uint16_t card = c565(C_CARD);
  tft.fillRect(x + 6, PY + 4, PW - 12, 32, card);
  if (i == CL) drawClaudeMark(x + 22, PY + 21, 10, a.brand, card);
  else drawGptMark(x + 22, PY + 21, 10, a.brand, card);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.setTextDatum(middle_left);
  tft.setTextColor(c565(C_FG));
  tft.drawString(a.name, x + 40, PY + 21);
  int nx = x + 40 + tft.textWidth(a.name) + 8;
  // status dot
  if (a.signedIn()) {
    bool stale = !a.haveData || millis() - a.lastOk > 15 * 60 * 1000UL;
    tft.fillCircle(nx + 3, PY + 22, 3, c565(a.lastError.length() && stale ? C_RED : stale ? C_AMBER : C_GREEN));
  }
  if (a.plan.length()) {
    tft.setFont(&fonts::FreeSansBold9pt7b);
    int w = tft.textWidth(a.plan) + 16;
    int px = x + PW - 12 - w;
    tft.fillRoundRect(px, PY + 10, w, 22, 11, c565(mix(C_CARD, a.brand, 0.22f)));
    tft.setTextColor(c565(a.brand));
    tft.setTextDatum(middle_center);
    tft.drawString(a.plan, px + w / 2, PY + 21);
  }
}

static void legendRow(int x, int y, RGB dot, const Win &w) {
  tft.fillCircle(x + 5, y, 4, c565(dot));
  tft.setFont(&fonts::FreeSansBold9pt7b);
  tft.setTextDatum(middle_left);
  tft.setTextColor(c565(C_FG));
  String l = w.label + " " + String((int)roundf(w.pct)) + "%";
  tft.drawString(l, x + 16, y);
  if (w.reset && timeValid()) {
    tft.setFont(&fonts::FreeSans9pt7b);
    tft.setTextColor(c565(C_DIM));
    tft.setTextDatum(middle_right);
    tft.drawString(fmtDur(w.reset - nowT()), x + PW - 32, y);
  }
}

static void drawLegend(int i) {
  Account &a = acc[i];
  int x = panelX(i) + 16, y = PY + 218;
  tft.fillRect(panelX(i) + 6, y - 6, PW - 12, PH - 218, c565(C_CARD));
  if (a.nWins > 0) legendRow(x, y + 6, levelColor(a.brand, a.wins[0].pct), a.wins[0]);
  if (a.nWins > 1) legendRow(x, y + 28, innerColor(a.brand, a.wins[1].pct), a.wins[1]);
  String foot;
  bool stale = !a.haveData || millis() - a.lastOk > 15 * 60 * 1000UL;
  if (a.lastError.length() && stale) foot = a.lastError;
  else if (a.extra.length()) foot = a.extra;
  else if (a.nWins > 1 && a.wins[1].reset && timeValid()) foot = "week resets " + fmtWhen(a.wins[1].reset);
  if (!a.haveData && !a.lastError.length()) foot = "Fetching usage...";
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextDatum(middle_center);
  tft.setTextColor(c565(a.lastError.length() && stale ? C_AMBER : mix(C_DIM, C_CARD, 0.25f)));
  tft.drawString(foot, panelX(i) + PW / 2, y + 48);
}

static void drawSignInBody(int i) {
  int x = panelX(i), cx = x + PW / 2;
  tft.fillRect(x + 6, PY + 38, PW - 12, PH - 44, c565(C_CARD));
  tft.setTextDatum(middle_center);
  if (i == CL) {
    String url = "http://" + WiFi.localIP().toString() + "/";
    tft.fillRoundRect(cx - 70, PY + 46, 140, 140, 10, TFT_WHITE);
    tft.qrcode(url.c_str(), cx - 62, PY + 54, 124, 2);
    tft.setFont(&fonts::FreeSansBold9pt7b);
    tft.setTextColor(c565(C_FG));
    tft.drawString("Scan to sign in", cx, PY + 206);
    tft.setFont(&fonts::FreeSans9pt7b);
    tft.setTextColor(c565(C_DIM));
    tft.drawString("or open", cx, PY + 230);
    tft.drawString(url, cx, PY + 252);
    return;
  }
  if (dev.active) {
    tft.fillRoundRect(cx - 54, PY + 42, 108, 108, 10, TFT_WHITE);
    tft.qrcode(OA_VERIFY_URL, cx - 48, PY + 48, 96, 3);
    tft.setFont(&fonts::FreeSans9pt7b);
    tft.setTextColor(c565(C_DIM));
    tft.drawString("auth.openai.com/codex/device", cx, PY + 166);
    tft.fillRoundRect(x + 18, PY + 182, PW - 36, 44, 10, c565(mix(C_CARD, C_GPT, 0.18f)));
    tft.setFont(&fonts::DejaVu24);
    tft.setTextColor(c565(C_FG));
    tft.drawString(dev.userCode, cx, PY + 205);
    tft.setFont(&fonts::FreeSans9pt7b);
    tft.setTextColor(c565(C_DIM));
    tft.drawString("Scan, sign in, enter this code", cx, PY + 248);
  } else {
    tft.setFont(&fonts::FreeSansBold12pt7b);
    tft.setTextColor(c565(C_FG));
    tft.drawString(dev.expired ? "Code expired" : "Not connected", cx, PY + 120);
    tft.setFont(&fonts::FreeSans9pt7b);
    tft.setTextColor(c565(dev.error.length() ? C_AMBER : C_DIM));
    if (dev.error.length()) tft.drawString(dev.error, cx, PY + 150);
    tft.setTextColor(c565(C_DIM));
    tft.drawString("Press BOOT for a new code", cx, PY + 176);
  }
}

static void drawPanel(int i) {
  tft.fillRoundRect(panelX(i), PY, PW, PH, 16, c565(C_CARD));
  drawPanelHeader(i);
  if (!acc[i].signedIn()) { drawSignInBody(i); return; }
  drawGauge(i);
  drawLegend(i);
}

static void animatePanel(int i) {
  Account &a = acc[i];
  float from[2] = {a.shown[0], a.shown[1]};
  float to[2] = {a.nWins > 0 ? a.wins[0].pct : 0, a.nWins > 1 ? a.wins[1].pct : 0};
  if (fabsf(to[0] - from[0]) < 0.5f && fabsf(to[1] - from[1]) < 0.5f) { a.shown[0] = to[0]; a.shown[1] = to[1]; drawGauge(i); return; }
  const int steps = 12;
  for (int s = 1; s <= steps; s++) {
    float t = (float)s / steps;
    t = 1 - (1 - t) * (1 - t) * (1 - t);
    a.shown[0] = from[0] + (to[0] - from[0]) * t;
    a.shown[1] = from[1] + (to[1] - from[1]) * t;
    drawGauge(i);
    server.handleClient();
  }
}

static void drawLogoBig(int cx, int cy) { drawClaudeMark(cx, cy, 30, C_CLAUDE, c565(C_BG)); }

static void screenMessage(const String &title, const String &sub) {
  tft.fillScreen(c565(C_BG));
  drawLogoBig(240, 120);
  tft.setTextDatum(middle_center);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.setTextColor(c565(C_FG));
  tft.drawString(title, 240, 186);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(c565(C_DIM));
  tft.drawString(sub, 240, 218);
}

static void screenAP() {
  tft.fillScreen(c565(C_BG));
  String qr = "WIFI:T:WPA;S:Claude-Usage;P:" + apPass + ";;";
  tft.fillRoundRect(16, 40, 200, 200, 14, TFT_WHITE);
  tft.qrcode(qr.c_str(), 26, 50, 180, 3);
  drawClaudeMark(254, 62, 14, C_CLAUDE, c565(C_BG));
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.setTextDatum(middle_left);
  tft.setTextColor(c565(C_FG));
  tft.drawString("WiFi setup", 276, 62);
  const char *lines[] = {"1. Scan to join the setup WiFi", "   Claude-Usage", nullptr, "2. Open http://192.168.4.1",
                         "   (usually opens by itself)", "3. Choose your WiFi network"};
  tft.setFont(&fonts::FreeSans9pt7b);
  for (int i = 0; i < 6; i++) {
    tft.setFont(lines[i] ? &fonts::FreeSans9pt7b : &fonts::FreeSansBold9pt7b);
    tft.setTextColor(c565(lines[i] ? C_DIM : C_FG));
    tft.drawString(lines[i] ? String(lines[i]) : "   password  " + apPass, 240, 104 + i * 24);
  }
  if (connectError.length()) {
    tft.setFont(&fonts::FreeSansBold9pt7b);
    tft.setTextColor(c565(C_AMBER));
    tft.drawString(connectError, 240, 104 + 6 * 24 + 8);
  }
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(c565(mix(C_DIM, C_BG, 0.35f)));
  tft.setTextDatum(middle_center);
  tft.drawString("Hold BOOT for 5 s to reset", 240, 290);
}

static void drawMain() {
  tft.fillScreen(c565(C_BG));
  drawTopBar();
  drawPanel(CL);
  drawPanel(OA);
}

// ---------------------------------------------------------------- storage
static void loadAccount(Account &a) {
  String p = a.pfx;
  a.refreshTok = prefs.getString((p + "rtok").c_str(), "");
  a.accessTok = prefs.getString((p + "atok").c_str(), "");
  a.accessExp = prefs.getUInt((p + "aexp").c_str(), 0);
  a.plan = prefs.getString((p + "plan").c_str(), "");
  a.acctId = prefs.getString((p + "acct").c_str(), "");
}
static void loadPrefs() {
  prefs.begin("claude", false);
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  loadAccount(acc[CL]);
  loadAccount(acc[OA]);
}

// ---------------------------------------------------------------- fetch orchestration
static void refreshAccount(int i) {
  Account &a = acc[i];
  if (!a.signedIn() || mode != M_RUN) return;
  a.nextFetchAt = millis() + POLL_MS;
  if (WiFi.status() != WL_CONNECTED) { a.lastError = "WiFi disconnected"; drawPanel(i); return; }
  tft.fillCircle(panelX(i) + PW - 8, PY + 8, 2, c565(a.brand));  // activity blip
  bool ok = i == CL ? clFetch() : oaFetch();
  if (ok) { a.haveData = true; a.lastOk = millis(); a.lastError = ""; }
  if (!a.signedIn()) {  // login revoked
    if (i == OA) startDeviceFlow();
    drawPanel(i);
    return;
  }
  drawPanelHeader(i);
  if (ok) animatePanel(i);
  drawLegend(i);
  tft.fillCircle(panelX(i) + PW - 8, PY + 8, 2, c565(C_CARD));
}

// ---------------------------------------------------------------- web UI
static const char PAGE_HEAD[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Usage display</title><style>
:root{--bg:#f5f4ee;--card:#fff;--fg:#1f1e1d;--dim:#6b6a65;--acc:#d97757;--gpt:#10a37f;--line:#e5e3da}
@media(prefers-color-scheme:dark){:root{--bg:#171615;--card:#242321;--fg:#f5f4ee;--dim:#9c9a92;--line:#38363a}}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 -apple-system,system-ui,sans-serif}
main{max-width:460px;margin:0 auto;padding:28px 16px}h1{font-size:22px;margin:0 0 4px}h2{font-size:18px;margin:0 0 6px}
.card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:20px;margin:16px 0}
p{color:var(--dim);margin:8px 0}label{display:block;font-weight:600;margin:12px 0 6px}
input{width:100%;box-sizing:border-box;padding:12px;border-radius:10px;border:1px solid var(--line);background:var(--bg);color:var(--fg);font-size:16px}
.btn{display:block;width:100%;box-sizing:border-box;text-align:center;padding:13px;border-radius:12px;border:0;background:var(--acc);color:#fff;font-weight:600;font-size:16px;text-decoration:none;margin-top:14px;cursor:pointer}
.gpt .btn{background:var(--gpt)}.ghost,.gpt .ghost{background:transparent;color:var(--fg);border:1px solid var(--line)}
.err{color:#e5484d}.ok{color:#3f9b56}.code{font:600 28px ui-monospace,monospace;letter-spacing:2px;text-align:center;padding:12px;border-radius:12px;background:var(--bg);margin:10px 0}
.bar{height:10px;background:var(--line);border-radius:5px;overflow:hidden;margin:6px 0 2px}.bar i{display:block;height:100%;background:var(--acc)}.gpt .bar i{background:var(--gpt)}
.row{display:flex;justify-content:space-between}small{color:var(--dim)}
</style></head><body><main><h1>Usage display</h1>)HTML";

static void sendPage(const String &body) {
  String s = FPSTR(PAGE_HEAD);
  s += body;
  s += "</main></body></html>";
  server.send(200, "text/html", s);
}

static void pageWifi() {
  int n = WiFi.scanNetworks();
  String opts;
  for (int i = 0; i < n && i < 25; i++) {
    String s = htmlEsc(WiFi.SSID(i));
    if (s.length() && opts.indexOf(">" + s + "<") < 0) opts += "<option>" + s + "</option>";
  }
  String b = connectError.length() ? "<p class=err>" + htmlEsc(connectError) + "</p>" : "";
  b += "<p>Pick the WiFi network the display should use.</p><div class=card><form method=post action=/wifi>"
       "<label>Network</label><input name=ssid list=nets autocomplete=off placeholder='Network name' value='" +
       htmlEsc(wifiSsid) + "'><datalist id=nets>" + opts +
       "</datalist><label>Password</label><input name=pass type=password autocomplete=off>"
       "<button class=btn>Connect</button></form></div>";
  sendPage(b);
}

static String usageBlock(Account &a) {
  String b;
  if (!a.nWins) return "<p>" + htmlEsc(a.lastError.length() ? a.lastError : String("No data yet")) + "</p>";
  for (int i = 0; i < a.nWins; i++) {
    int p = (int)roundf(a.wins[i].pct);
    b += "<div class=row><b>" + htmlEsc(a.wins[i].label) + "</b><span>" + String(p) + "%</span></div><div class=bar><i style='width:" +
         String(constrain(p, 0, 100)) + "%'></i></div>";
    if (a.wins[i].reset) b += "<small>resets in " + fmtDur(a.wins[i].reset - nowT()) + " &middot; " + fmtWhen(a.wins[i].reset) + "</small><br>";
  }
  if (a.extra.length()) b += "<p>" + htmlEsc(a.extra) + "</p>";
  return b;
}

static void pageHome(const String &msg) {
  String b = msg.length() ? "<p class=err>" + htmlEsc(msg) + "</p>" : "";
  // Claude
  Account &c = acc[CL];
  b += "<div class=card><h2>Claude" + (c.plan.length() ? " &middot; " + htmlEsc(c.plan) : String("")) + "</h2>";
  if (c.signedIn()) {
    b += usageBlock(c) + "<form method=post action=/cl/signout><button class='btn ghost'>Sign out of Claude</button></form>";
  } else {
    if (!pkceVerifier.length()) { pkceVerifier = randomToken(32); pkceState = randomToken(24); }
    String url = String(CL_AUTH_URL) + "?code=true&client_id=" + CL_CLIENT_ID + "&response_type=code&redirect_uri=" +
                 urlEncode(CL_REDIRECT) + "&scope=" + urlEncode(CL_SCOPE) + "&code_challenge=" + pkceChallenge(pkceVerifier) +
                 "&code_challenge_method=S256&state=" + pkceState;
    b += "<p>Read-only sign-in: the display can see usage limits but can't send prompts.</p>"
         "<a class=btn target=_blank rel=noopener href='" + url + "'>1. Sign in with Claude</a>"
         "<form method=post action=/cl/code><label>2. Paste the code Claude shows</label><input name=code autocomplete=off "
         "autocapitalize=off spellcheck=false placeholder='code#state'><button class=btn>Finish</button></form>";
  }
  b += "</div>";
  // ChatGPT
  Account &o = acc[OA];
  b += "<div class='card gpt'><h2>ChatGPT" + (o.plan.length() ? " &middot; " + htmlEsc(o.plan) : String("")) + "</h2>";
  if (o.signedIn()) {
    b += usageBlock(o) + "<p>These are the Codex limits included with the ChatGPT plan.</p>"
         "<form method=post action=/oa/signout><button class='btn ghost'>Sign out of ChatGPT</button></form>";
  } else if (dev.active) {
    b += "<p>On the phone or computer of the ChatGPT account owner:</p><a class=btn target=_blank rel=noopener href='" +
         String(OA_VERIFY_URL) + "'>1. Open the ChatGPT device page</a><p>2. Sign in and enter this code:</p><div class=code>" +
         htmlEsc(dev.userCode) + "</div><p>The display connects by itself within a few seconds. The code works for 15 minutes.</p>";
  } else {
    if (dev.error.length()) b += "<p class=err>" + htmlEsc(dev.error) + "</p>";
    b += "<form method=post action=/oa/start><button class=btn>Get a sign-in code</button></form>";
  }
  b += "</div><form method=post action=/refresh><button class='btn ghost'>Refresh now</button></form>"
       "<form method=post action=/forget onsubmit=\"return confirm('Forget WiFi and sign out of both?')\"><button class='btn ghost'>Reset everything</button></form>"
       "<p><small>WiFi " + htmlEsc(wifiSsid) + "</small></p>";
  sendPage(b);
}

static void redirectHome() {
  server.sendHeader("Location", "/");
  server.send(303);
}

static void setupRoutes() {
  server.on("/", HTTP_GET, [] {
    if (mode == M_AP) pageWifi();
    else pageHome("");
  });
  server.on("/wifi", HTTP_POST, [] {
    prefs.putString("ssid", server.arg("ssid"));
    prefs.putString("pass", server.arg("pass"));
    sendPage("<div class=card><b>Saved.</b><p>The display is restarting and joining <b>" + htmlEsc(server.arg("ssid")) +
             "</b>. Reconnect this device to your normal WiFi, then follow the screen.</p></div>");
    delay(1500);
    ESP.restart();
  });
  server.on("/cl/code", HTTP_POST, [] {
    String raw = server.arg("code");
    raw.trim();
    int hash = raw.indexOf('#');
    String code = hash >= 0 ? raw.substring(0, hash) : raw;
    String state = hash >= 0 ? raw.substring(hash + 1) : pkceState;
    if (!pkceVerifier.length() || state != pkceState) {
      pkceVerifier = "";
      pageHome("That code is from an older sign-in link. Use the new link below.");
      return;
    }
    JsonDocument d;
    d["grant_type"] = "authorization_code";
    d["code"] = code;
    d["state"] = state;
    d["client_id"] = CL_CLIENT_ID;
    d["redirect_uri"] = CL_REDIRECT;
    d["code_verifier"] = pkceVerifier;
    String body, resp;
    serializeJson(d, body);
    int rc = https("POST", CL_TOKEN_URL, body, nullptr, {{"User-Agent", "claude-usage-cyd/1.0"}}, resp);
    Serial.printf("[claude] exchange -> %d\n", rc);
    pkceVerifier = "";
    if (rc != 200 || !clStoreTokens(resp)) { pageHome("Claude sign-in failed (" + String(rc) + "). Try again."); return; }
    redirectHome();
    acc[CL].nextFetchAt = millis();
    drawPanel(CL);
  });
  server.on("/oa/start", HTTP_POST, [] {
    startDeviceFlow();
    redirectHome();
    if (mode == M_RUN) drawPanel(OA);
  });
  server.on("/cl/signout", HTTP_POST, [] { signOut(acc[CL]); redirectHome(); drawPanel(CL); });
  server.on("/oa/signout", HTTP_POST, [] { signOut(acc[OA]); dev = DeviceFlow(); redirectHome(); drawPanel(OA); });
  server.on("/refresh", HTTP_POST, [] { acc[CL].nextFetchAt = acc[OA].nextFetchAt = millis(); redirectHome(); });
  server.on("/forget", HTTP_POST, [] {
    prefs.clear();
    sendPage("<div class=card>Reset. The display is restarting into setup mode.</div>");
    delay(1000);
    ESP.restart();
  });
  server.onNotFound([] {
    if (mode == M_AP) {
      server.sendHeader("Location", "http://192.168.4.1/");
      server.send(302);
    } else server.send(404, "text/plain", "not found");
  });
}

// ---------------------------------------------------------------- modes
static void enterAP() {
  mode = M_AP;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);
  char p[9];
  snprintf(p, sizeof p, "%08u", (unsigned)(esp_random() % 100000000UL));
  apPass = p;
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP("Claude-Usage", apPass.c_str());
  dns.start(53, "*", IPAddress(192, 168, 4, 1));
  screenAP();
}

static bool connectWifi() {
  screenMessage("Connecting to WiFi", wifiSsid);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);
  if (WiFi.status() != WL_CONNECTED) { connectError = "Couldn't join " + wifiSsid; return false; }
  WiFi.setAutoReconnect(true);
  Serial.printf("[wifi] %s\n", WiFi.localIP().toString().c_str());
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
  t0 = millis();
  while (!timeValid() && millis() - t0 < 10000) delay(100);
  MDNS.begin(HOSTNAME);
  MDNS.addService("http", "tcp", 80);
  return true;
}

static void enterRun() {
  mode = M_RUN;
  if (!acc[OA].signedIn()) startDeviceFlow();
  drawMain();
  acc[CL].nextFetchAt = millis();
  acc[OA].nextFetchAt = millis() + 2000;
}

// ---------------------------------------------------------------- main
void setup() {
  Serial.begin(115200);
  pinMode(33, OUTPUT);  // touch controller CS: keep it off the shared SPI bus
  digitalWrite(33, HIGH);
  pinMode(0, INPUT_PULLUP);
  tft.init();
  tft.setRotation(1);
  tft.setBrightness(210);
  gauge.setColorDepth(16);
  gauge.createSprite(G, G);
  loadPrefs();
  setupRoutes();
  if (!wifiSsid.length() || !connectWifi()) enterAP();
  else enterRun();
  server.begin();
}

void loop() {
  server.handleClient();
  if (mode == M_AP) dns.processNextRequest();

  static uint32_t downAt = 0;
  if (digitalRead(0) == LOW) {
    if (!downAt) downAt = millis();
    if (millis() - downAt > 5000) {
      screenMessage("Reset", "Forgetting WiFi and both sign-ins");
      prefs.clear();
      delay(1200);
      ESP.restart();
    }
  } else if (downAt) {
    if (millis() - downAt > 50 && mode == M_RUN) {
      acc[CL].nextFetchAt = millis();
      acc[OA].nextFetchAt = millis();
      if (!acc[OA].signedIn() && !dev.active) { startDeviceFlow(); drawPanel(OA); }
    }
    downAt = 0;
  }

  if (mode != M_RUN) return;

  // ChatGPT device-code login in progress
  if (!acc[OA].signedIn() && dev.active) {
    bool wasActive = dev.active;
    if (pollDeviceFlow()) {
      acc[OA].nextFetchAt = millis();
      drawPanel(OA);
    } else if (wasActive && !dev.active) {
      drawPanel(OA);  // expired or failed
    }
  }

  for (int i = 0; i < 2; i++)
    if (acc[i].signedIn() && (long)(millis() - acc[i].nextFetchAt) >= 0) refreshAccount(i);

  static uint32_t lastTick = 0;
  static int lastMin = -1;
  if (millis() - lastTick > 1000) {
    lastTick = millis();
    time_t n = nowT();
    struct tm lt;
    localtime_r(&n, &lt);
    if (lt.tm_min != lastMin) {
      lastMin = lt.tm_min;
      drawTopBar();
      for (int i = 0; i < 2; i++)
        if (acc[i].signedIn()) { drawPanelHeader(i); drawLegend(i); }
    }
  }
}
