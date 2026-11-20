// Claude usage display for the 4" CYD (ESP32-32E, ST7796 480x320).
// Standalone: joins WiFi, signs in to Claude with its own OAuth login
// (scope user:profile only, so it can read usage but not run prompts),
// and polls the same usage endpoint Claude Code's /usage uses.
//
// First boot: the board opens a WiFi hotspot; scan the QR on screen, pick your
// network, then follow "Sign in with Claude" on the page it serves.
// BOOT button: tap = refresh now, hold 5 s = forget everything.
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
static const char *CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
static const char *AUTH_URL = "https://claude.com/cai/oauth/authorize";
static const char *TOKEN_URL = "https://platform.claude.com/v1/oauth/token";
static const char *REDIRECT_URI = "https://platform.claude.com/oauth/code/callback";
static const char *SCOPE = "user:profile";
static const char *USAGE_URL = "https://api.anthropic.com/api/oauth/usage";
static const char *PROFILE_URL = "https://api.anthropic.com/api/oauth/profile";
static const uint32_t POLL_MS = 5UL * 60 * 1000;

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
static const RGB C_TRACK = {0x38, 0x36, 0x33};
static const RGB C_FG = {0xF5, 0xF4, 0xEE};
static const RGB C_DIM = {0x9C, 0x9A, 0x92};
static const RGB C_CLAUDE = {0xD9, 0x77, 0x57};
static const RGB C_AMBER = {0xE8, 0xA2, 0x3A};
static const RGB C_RED = {0xE5, 0x48, 0x4D};
static const RGB C_GREEN = {0x6B, 0xC4, 0x7E};
static inline uint16_t c565(RGB c) { return lgfx::color565(c.r, c.g, c.b); }
static inline RGB mix(RGB a, RGB b, float t) {
  return {uint8_t(a.r + (b.r - a.r) * t), uint8_t(a.g + (b.g - a.g) * t), uint8_t(a.b + (b.b - a.b) * t)};
}
static RGB levelColor(float p) { return p >= 90 ? C_RED : p >= 75 ? C_AMBER : C_CLAUDE; }

// ---------------------------------------------------------------- state
enum Mode { M_BOOT, M_AP, M_SIGNIN, M_RUN };
static Mode mode = M_BOOT;
static Preferences prefs;
static WebServer server(80);
static DNSServer dns;
static String apPass, wifiSsid, wifiPass, refreshTok, accessTok, plan;
static uint32_t accessExp = 0;
static String pkceVerifier, pkceState;
static String lastError;
static String connectError;

struct Win { String key, label; float pct; time_t reset; };
static Win wins[6];
static int nWins = 0;
static float shown[2] = {0, 0};
static bool haveData = false;
static uint32_t lastFetch = 0, lastOk = 0, nextFetchAt = 0;

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
// "2026-11-20T12:59:59.874+00:00" / "...Z" -> unix seconds
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
    else o += c;
  }
  return o;
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
  struct tm lt;
  localtime_r(&t, &lt);
  char b[24];
  time_t n = nowT();
  struct tm ln;
  localtime_r(&n, &ln);
  if (lt.tm_yday == ln.tm_yday && lt.tm_year == ln.tm_year) strftime(b, sizeof b, "today %H:%M", &lt);
  else strftime(b, sizeof b, "%a %H:%M", &lt);
  return b;
}

// ---------------------------------------------------------------- drawing
static void drawLogo(int cx, int cy, int r, RGB col) {
  for (int i = 0; i < 12; i++) {
    float a = i * PI / 6 + 0.13f;
    float len = (i % 3 == 0) ? r : r * 0.78f;
    tft.drawWedgeLine(cx, cy, cx + len * cosf(a), cy + len * sinf(a), 2.2f, 1.0f, c565(col));
  }
  tft.fillCircle(cx, cy, 3, c565(col));
}

static void drawWifiIcon(int x, int y) {  // x,y = bottom-left
  int rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -200;
  int bars = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : rssi > -90 ? 1 : 0;
  for (int i = 0; i < 4; i++) {
    int h = 5 + i * 4;
    tft.fillRoundRect(x + i * 6, y - h, 4, h, 1, c565(i < bars ? C_FG : C_TRACK));
  }
}

static void drawHeader() {
  tft.fillRect(0, 0, 480, 50, c565(C_BG));
  drawLogo(26, 26, 13, C_CLAUDE);
  tft.setFont(&fonts::FreeSansBold18pt7b);
  tft.setTextDatum(middle_left);
  tft.setTextColor(c565(C_FG));
  tft.drawString("Claude", 48, 26);
  int x = 48 + tft.textWidth("Claude") + 12;
  if (plan.length()) {
    tft.setFont(&fonts::FreeSansBold9pt7b);
    int w = tft.textWidth(plan) + 22;
    tft.fillRoundRect(x, 14, w, 24, 12, c565(mix(C_BG, C_CLAUDE, 0.22f)));
    tft.drawRoundRect(x, 14, w, 24, 12, c565(mix(C_BG, C_CLAUDE, 0.6f)));
    tft.setTextColor(c565(C_CLAUDE));
    tft.setTextDatum(middle_center);
    tft.drawString(plan, x + w / 2, 27);
  }
  if (timeValid()) {
    char b[8];
    time_t n = nowT();
    struct tm lt;
    localtime_r(&n, &lt);
    strftime(b, sizeof b, "%H:%M", &lt);
    tft.setFont(&fonts::FreeSans18pt7b);
    tft.setTextDatum(middle_right);
    tft.setTextColor(c565(C_FG));
    tft.drawString(b, 468, 26);
    drawWifiIcon(468 - tft.textWidth(b) - 36, 34);
  } else {
    drawWifiIcon(444, 34);
  }
}

// Anti-aliased 270-degree ring gauge, open at the bottom.
static const int G = 184;
static void renderGauge(float pct, const char *label, RGB col) {
  const float cx = (G - 1) / 2.0f, cy = (G - 1) / 2.0f, rm = 79, hw = 10;
  const float a0 = 135, sweep = 270;
  const float a1 = a0 + sweep * constrain(pct, 0, 100) / 100.0f;
  gauge.fillScreen(c565(C_CARD));
  auto capDist = [&](float px, float py, float adeg) {
    float r = adeg * DEG_TO_RAD;
    float dx = px - (cx + rm * cosf(r)), dy = py - (cy + rm * sinf(r));
    return sqrtf(dx * dx + dy * dy);
  };
  RGB dark = mix(col, C_CARD, 0.35f);
  for (int y = 0; y < G; y++) {
    float dy = y - cy;
    for (int x = 0; x < G; x++) {
      float dx = x - cx;
      float r = sqrtf(dx * dx + dy * dy);
      float band = fabsf(r - rm);
      if (band > hw + 1.5f) continue;
      float a = atan2f(dy, dx) * RAD_TO_DEG;
      if (a < 0) a += 360;
      if (a < a0) a += 360;  // a in [135, 495)
      float dT = (a <= a0 + sweep) ? band : fminf(capDist(x, y, a0), capDist(x, y, a0 + sweep));
      float cT = constrain(hw + 0.5f - dT, 0.0f, 1.0f);
      if (cT <= 0) continue;
      RGB px = mix(C_CARD, C_TRACK, cT);
      if (pct > 0.3f) {
        float dF = (a <= a1) ? band : fminf(capDist(x, y, a0), capDist(x, y, a1));
        float cF = constrain(hw + 0.5f - dF, 0.0f, 1.0f);
        if (cF > 0) {
          float t = constrain((a - a0) / fmaxf(a1 - a0, 1.0f), 0.0f, 1.0f);
          px = mix(px, mix(dark, col, t), cF);
        }
      }
      gauge.drawPixel(x, y, c565(px));
    }
  }
  // number
  char num[8];
  snprintf(num, sizeof num, "%d", (int)roundf(pct));
  gauge.setFont(&fonts::DejaVu56);
  int wn = gauge.textWidth(num);
  gauge.setFont(&fonts::DejaVu24);
  int wp = gauge.textWidth("%");
  int x0 = (G - wn - wp - 2) / 2, base = (int)cy + 20;
  gauge.setTextDatum(baseline_left);
  gauge.setFont(&fonts::DejaVu56);
  gauge.setTextColor(c565(C_FG));
  gauge.drawString(num, x0, base);
  gauge.setFont(&fonts::DejaVu24);
  gauge.setTextColor(c565(C_DIM));
  gauge.drawString("%", x0 + wn + 2, base - 22);
  // label in the gap
  gauge.setFont(&fonts::FreeSansBold9pt7b);
  gauge.setTextDatum(middle_center);
  gauge.setTextColor(c565(C_DIM));
  gauge.drawString(label, (int)cx, (int)cy + 66);
}

static const int CARD_Y = 56, CARD_H = 238, CARD_W = 222;
static int cardX(int i) { return 12 + i * (CARD_W + 12); }

static void drawCardText(int i) {
  int x = cardX(i), cx = x + CARD_W / 2;
  tft.fillRect(x + 8, CARD_Y + 192, CARD_W - 16, 42, c565(C_CARD));
  if (i >= nWins || !wins[i].reset || !timeValid()) return;
  long left = (long)(wins[i].reset - nowT());
  String a = "resets in ", b = fmtDur(left);
  tft.setFont(&fonts::FreeSans9pt7b);
  int wa = tft.textWidth(a);
  tft.setFont(&fonts::FreeSansBold9pt7b);
  int wb = tft.textWidth(b);
  int sx = cx - (wa + wb) / 2;
  tft.setTextDatum(middle_left);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(c565(C_DIM));
  tft.drawString(a, sx, CARD_Y + 204);
  tft.setFont(&fonts::FreeSansBold9pt7b);
  tft.setTextColor(c565(C_FG));
  tft.drawString(b, sx + wa, CARD_Y + 204);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(c565(mix(C_DIM, C_CARD, 0.3f)));
  tft.setTextDatum(middle_center);
  tft.drawString(fmtWhen(wins[i].reset), cx, CARD_Y + 224);
}

static const char *shortLabel(int i) {
  if (i >= nWins) return "";
  if (wins[i].key == "five_hour") return "5-HOUR";
  if (wins[i].key == "seven_day") return "WEEKLY";
  return "LIMIT";
}

static void drawGauge(int i, float pct) {
  RGB col = levelColor(i < nWins ? wins[i].pct : 0);
  renderGauge(pct, shortLabel(i), col);
  gauge.pushSprite(cardX(i) + (CARD_W - G) / 2, CARD_Y + 8);
}

static void drawFooter() {
  tft.fillRect(0, 296, 480, 24, c565(C_BG));
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextDatum(middle_left);
  uint32_t age = (millis() - lastOk) / 1000;
  if (lastError.length() && (!haveData || millis() - lastOk > 15 * 60 * 1000UL)) {
    tft.fillCircle(18, 308, 4, c565(C_RED));
    tft.setTextColor(c565(C_AMBER));
    tft.drawString(lastError, 28, 308);
  } else if (haveData) {
    tft.fillCircle(18, 308, 4, c565(age < 600 ? C_GREEN : C_AMBER));
    tft.setTextColor(c565(C_DIM));
    String s = age < 60 ? String("Updated just now") : "Updated " + String(age / 60) + " min ago";
    tft.drawString(s, 28, 308);
  } else {
    tft.setTextColor(c565(C_DIM));
    tft.drawString("Fetching usage...", 28, 308);
  }
  // extra limits (per-model weekly etc.)
  String extra;
  for (int i = 2; i < nWins; i++) {
    if (extra.length()) extra += "   ";
    extra += wins[i].label + " " + String((int)roundf(wins[i].pct)) + "%";
  }
  if (extra.length()) {
    tft.setTextDatum(middle_right);
    tft.setTextColor(c565(C_DIM));
    tft.drawString(extra, 468, 308);
  }
}

static void drawMainFrame() {
  tft.fillScreen(c565(C_BG));
  drawHeader();
  for (int i = 0; i < 2; i++) {
    tft.fillRoundRect(cardX(i), CARD_Y, CARD_W, CARD_H, 18, c565(C_CARD));
    drawGauge(i, shown[i]);
    drawCardText(i);
  }
  drawFooter();
}

static void animateTo() {
  float from[2] = {shown[0], shown[1]};
  float to[2] = {nWins > 0 ? wins[0].pct : 0, nWins > 1 ? wins[1].pct : 0};
  const int steps = 14;
  for (int s = 1; s <= steps; s++) {
    float t = (float)s / steps;
    t = 1 - (1 - t) * (1 - t) * (1 - t);  // ease-out cubic
    for (int i = 0; i < 2; i++) {
      if (fabsf(to[i] - from[i]) < 0.5f && s > 1) continue;
      shown[i] = from[i] + (to[i] - from[i]) * t;
      drawGauge(i, shown[i]);
    }
  }
  shown[0] = to[0];
  shown[1] = to[1];
}

static void drawCentered(const char *s, int y, const lgfx::IFont *f, RGB c) {
  tft.setFont(f);
  tft.setTextDatum(middle_center);
  tft.setTextColor(c565(c));
  tft.drawString(s, 240, y);
}

static void screenMessage(const String &title, const String &sub) {
  tft.fillScreen(c565(C_BG));
  drawLogo(240, 120, 30, C_CLAUDE);
  drawCentered(title.c_str(), 186, &fonts::FreeSansBold12pt7b, C_FG);
  drawCentered(sub.c_str(), 218, &fonts::FreeSans9pt7b, C_DIM);
}

static void screenWithQR(const String &qr, const char *title, const String lines[], int n) {
  tft.fillScreen(c565(C_BG));
  tft.fillRoundRect(16, 40, 200, 200, 14, TFT_WHITE);
  tft.qrcode(qr.c_str(), 26, 50, 180, 3);
  drawLogo(254, 62, 14, C_CLAUDE);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.setTextDatum(middle_left);
  tft.setTextColor(c565(C_FG));
  tft.drawString(title, 276, 62);
  tft.setFont(&fonts::FreeSans9pt7b);
  for (int i = 0; i < n; i++) {
    bool strong = lines[i].startsWith("*");
    tft.setTextColor(c565(strong ? C_FG : C_DIM));
    tft.setFont(strong ? &fonts::FreeSansBold9pt7b : &fonts::FreeSans9pt7b);
    tft.drawString(strong ? lines[i].substring(1) : lines[i], 240, 104 + i * 24);
  }
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(c565(mix(C_DIM, C_BG, 0.35f)));
  tft.setTextDatum(middle_center);
  tft.drawString("Hold BOOT for 5 s to reset", 240, 290);
}

static void screenAP() {
  String qr = "WIFI:T:WPA;S:Claude-Usage;P:" + apPass + ";;";
  String lines[] = {"1. Scan to join the setup WiFi", "   Claude-Usage", "*   password  " + apPass,
                    "2. Open http://192.168.4.1", "   (usually opens by itself)", "3. Choose your WiFi network",
                    connectError.length() ? "*" + connectError : String("")};
  screenWithQR(qr, "WiFi setup", lines, 7);
}

static void screenSignIn() {
  String ip = WiFi.localIP().toString();
  String qr = "http://" + ip + "/";
  String lines[] = {"Connected to " + wifiSsid, "", "1. Scan, or on any device open", "*   http://" + ip,
                    "   or http://" + String(HOSTNAME) + ".local", "2. Tap Sign in with Claude",
                    "3. Paste the code back there"};
  screenWithQR(qr, "Sign in", lines, 7);
}

// ---------------------------------------------------------------- storage
static void loadPrefs() {
  prefs.begin("claude", false);
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  refreshTok = prefs.getString("rtok", "");
  accessTok = prefs.getString("atok", "");
  accessExp = prefs.getUInt("aexp", 0);
  plan = prefs.getString("plan", "");
}
static void saveTokens() {
  prefs.putString("rtok", refreshTok);
  prefs.putString("atok", accessTok);
  prefs.putUInt("aexp", accessExp);
}
static void signOut() {
  refreshTok = accessTok = plan = "";
  accessExp = 0;
  prefs.remove("rtok");
  prefs.remove("atok");
  prefs.remove("aexp");
  prefs.remove("plan");
}

// ---------------------------------------------------------------- network
static int httpsJson(const char *method, const char *url, const String &body, String &resp, bool auth) {
  WiFiClientSecure client;
  client.setCACert(ROOT_CAS);
  client.setTimeout(15);
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, url)) return -100;
  http.addHeader("Accept", "application/json");
  http.addHeader("User-Agent", "claude-usage-cyd/1.0");
  if (auth) {
    http.addHeader("Authorization", "Bearer " + accessTok);
    http.addHeader("anthropic-beta", "oauth-2025-04-20");
  }
  int code;
  if (!strcmp(method, "POST")) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(body);
  } else {
    code = http.GET();
  }
  resp = code > 0 ? http.getString() : String(http.errorToString(code));
  http.end();
  return code;
}

static bool storeTokenResponse(const String &resp) {
  JsonDocument d;
  if (deserializeJson(d, resp)) return false;
  const char *at = d["access_token"];
  if (!at) return false;
  accessTok = at;
  if (d["refresh_token"].is<const char *>()) refreshTok = d["refresh_token"].as<const char *>();
  uint32_t exp = d["expires_in"] | 3600;
  accessExp = timeValid() ? nowT() + exp : 0;
  saveTokens();
  Serial.printf("[auth] token ok, scope=%s, expires_in=%u\n", (const char *)(d["scope"] | "?"), exp);
  return true;
}

static bool refreshAccess() {
  if (!refreshTok.length()) return false;
  JsonDocument d;
  d["grant_type"] = "refresh_token";
  d["refresh_token"] = refreshTok;
  d["client_id"] = CLIENT_ID;
  d["scope"] = SCOPE;
  String body, resp;
  serializeJson(d, body);
  int code = httpsJson("POST", TOKEN_URL, body, resp, false);
  Serial.printf("[auth] refresh -> %d\n", code);
  if (code == 200 && storeTokenResponse(resp)) return true;
  Serial.println(resp.substring(0, 300));
  if (code == 400 || code == 401) {
    lastError = "Sign-in expired - open http://" + WiFi.localIP().toString();
    signOut();
  } else {
    lastError = "Token refresh failed (" + String(code) + ")";
  }
  return false;
}

static void fetchPlan() {
  String resp;
  int code = httpsJson("GET", PROFILE_URL, "", resp, true);
  Serial.printf("[profile] %d\n", code);
  if (code != 200) return;
  JsonDocument d;
  if (deserializeJson(d, resp)) return;
  String t = d["organization"]["organization_type"] | "";
  if (t.indexOf("max") >= 0) plan = "Max";
  else if (t.indexOf("pro") >= 0) plan = "Pro";
  else if (t.indexOf("team") >= 0) plan = "Team";
  else if (t.indexOf("enterprise") >= 0) plan = "Enterprise";
  else if (d["account"]["has_claude_max"] | false) plan = "Max";
  else if (d["account"]["has_claude_pro"] | false) plan = "Pro";
  Serial.printf("[profile] org type '%s' -> plan '%s'\n", t.c_str(), plan.c_str());
  prefs.putString("plan", plan);
}

static String labelFor(const String &k) {
  if (k == "five_hour") return "5-hour";
  if (k == "seven_day") return "Weekly";
  if (k == "seven_day_opus") return "Opus";
  if (k == "seven_day_sonnet") return "Sonnet";
  String s = k;
  s.replace("seven_day_", "");
  s.replace("_", " ");
  if (s.length()) s[0] = toupper(s[0]);
  return s;
}

static bool fetchUsage() {
  lastFetch = millis();
  if (WiFi.status() != WL_CONNECTED) { lastError = "WiFi disconnected"; return false; }
  if (!accessTok.length() || (accessExp && timeValid() && nowT() > (time_t)accessExp - 300)) {
    if (!refreshAccess()) return false;
  }
  String resp;
  int code = httpsJson("GET", USAGE_URL, "", resp, true);
  if (code == 401) {
    if (!refreshAccess()) return false;
    code = httpsJson("GET", USAGE_URL, "", resp, true);
  }
  Serial.printf("[usage] %d %s\n", code, resp.substring(0, 400).c_str());
  if (code == 429) { lastError = "Rate limited, retrying later"; nextFetchAt = millis() + 10 * 60 * 1000UL; return false; }
  if (code != 200) { lastError = "Usage request failed (" + String(code) + ")"; return false; }
  JsonDocument d;
  if (deserializeJson(d, resp)) { lastError = "Bad usage response"; return false; }
  Win tmp[6];
  int n = 0;
  // fixed order first, then anything else that looks like a limit
  const char *order[] = {"five_hour", "seven_day", "seven_day_opus", "seven_day_sonnet"};
  for (const char *k : order) {
    JsonVariant v = d[k];
    if (n < 6 && v.is<JsonObject>() && !v["utilization"].isNull())
      tmp[n++] = {k, labelFor(k), v["utilization"].as<float>(), parseIso(v["resets_at"] | "")};
  }
  for (JsonPair p : d.as<JsonObject>()) {
    String k = p.key().c_str();
    bool known = false;
    for (const char *o : order) known |= (k == o);
    JsonVariant v = p.value();
    if (!known && n < 6 && k.startsWith("seven_day") && v.is<JsonObject>() && !v["utilization"].isNull())
      tmp[n++] = {k, labelFor(k), v["utilization"].as<float>(), parseIso(v["resets_at"] | "")};
  }
  if (!n) { lastError = "No limits in response"; return false; }
  for (int i = 0; i < n; i++) wins[i] = tmp[i];
  nWins = n;
  haveData = true;
  lastOk = millis();
  lastError = "";
  return true;
}

// ---------------------------------------------------------------- web UI
static const char PAGE_HEAD[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Claude usage display</title><style>
:root{--bg:#f5f4ee;--card:#fff;--fg:#1f1e1d;--dim:#6b6a65;--acc:#d97757;--line:#e5e3da}
@media(prefers-color-scheme:dark){:root{--bg:#171615;--card:#242321;--fg:#f5f4ee;--dim:#9c9a92;--line:#38363a}}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 -apple-system,system-ui,sans-serif}
main{max-width:440px;margin:0 auto;padding:28px 16px}h1{font-size:22px;margin:0 0 4px}
.card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:20px;margin:16px 0}
p{color:var(--dim);margin:8px 0}label{display:block;font-weight:600;margin:12px 0 6px}
input,select{width:100%;box-sizing:border-box;padding:12px;border-radius:10px;border:1px solid var(--line);background:var(--bg);color:var(--fg);font-size:16px}
.btn{display:block;width:100%;box-sizing:border-box;text-align:center;padding:13px;border-radius:12px;border:0;background:var(--acc);color:#fff;font-weight:600;font-size:16px;text-decoration:none;margin-top:16px;cursor:pointer}
.ghost{background:transparent;color:var(--fg);border:1px solid var(--line)}.err{color:#e5484d}.ok{color:#3f9b56}
.bar{height:10px;background:var(--line);border-radius:5px;overflow:hidden;margin:6px 0 2px}.bar i{display:block;height:100%;background:var(--acc)}
.row{display:flex;justify-content:space-between}small{color:var(--dim)}
</style></head><body><main><h1>Claude usage display</h1>)HTML";
static const char PAGE_TAIL[] PROGMEM = "</main></body></html>";

static void sendPage(const String &body) {
  String s = FPSTR(PAGE_HEAD);
  s += body;
  s += FPSTR(PAGE_TAIL);
  server.send(200, "text/html", s);
}

static void pageWifi() {
  int n = WiFi.scanNetworks();
  String opts;
  for (int i = 0; i < n && i < 25; i++) {
    String s = WiFi.SSID(i);
    if (!s.length() || opts.indexOf(">" + htmlEsc(s) + "<") >= 0) continue;
    opts += "<option>" + htmlEsc(s) + "</option>";
  }
  String b = "<p>Pick the WiFi network the display should use.</p><div class=card><form method=post action=/wifi>"
             "<label>Network</label><input name=ssid list=nets autocomplete=off placeholder='Network name' value='" +
             htmlEsc(wifiSsid) + "'><datalist id=nets>" + opts +
             "</datalist><label>Password</label><input name=pass type=password autocomplete=off>"
             "<button class=btn>Connect</button></form></div>";
  if (connectError.length()) b = "<p class=err>" + htmlEsc(connectError) + "</p>" + b;
  sendPage(b);
}

static void pageSignIn(const String &msg) {
  pkceVerifier = randomToken(32);
  pkceState = randomToken(24);
  String url = String(AUTH_URL) + "?code=true&client_id=" + CLIENT_ID + "&response_type=code&redirect_uri=" +
               urlEncode(REDIRECT_URI) + "&scope=" + urlEncode(SCOPE) + "&code_challenge=" + pkceChallenge(pkceVerifier) +
               "&code_challenge_method=S256&state=" + pkceState;
  String b = msg.length() ? "<p class=err>" + htmlEsc(msg) + "</p>" : "";
  b += "<p>The display gets its own read-only sign-in (<b>user:profile</b>): it can read your usage limits but "
       "can't send prompts, and it doesn't touch your Claude Code login.</p>"
       "<div class=card><b>1.</b> Sign in and approve<a class=btn target=_blank rel=noopener href='" + url +
       "'>Sign in with Claude</a></div><div class=card><b>2.</b> Paste the code Claude shows you"
       "<form method=post action=/code><input name=code autocomplete=off autocapitalize=off spellcheck=false "
       "placeholder='code#state'><button class=btn>Finish</button></form></div>";
  sendPage(b);
}

static void pageStatus() {
  String b = "<p>Signed in" + (plan.length() ? " &middot; " + htmlEsc(plan) : String("")) + " &middot; WiFi " + htmlEsc(wifiSsid) + "</p><div class=card>";
  if (!nWins) b += "<p>" + htmlEsc(lastError.length() ? lastError : String("No data yet")) + "</p>";
  for (int i = 0; i < nWins; i++) {
    int p = (int)roundf(wins[i].pct);
    b += "<div class=row><b>" + htmlEsc(wins[i].label) + "</b><span>" + String(p) + "%</span></div><div class=bar><i style='width:" +
         String(constrain(p, 0, 100)) + "%'></i></div>";
    if (wins[i].reset) b += "<small>resets in " + fmtDur(wins[i].reset - nowT()) + " &middot; " + fmtWhen(wins[i].reset) + "</small>";
    b += "<br><br>";
  }
  b += "</div><form method=post action=/refresh><button class=btn>Refresh now</button></form>"
       "<form method=post action=/signout><button class='btn ghost'>Sign out</button></form>"
       "<form method=post action=/forget onsubmit=\"return confirm('Forget WiFi and sign out?')\"><button class='btn ghost'>Reset everything</button></form>";
  sendPage(b);
}

static void redirectHome() {
  server.sendHeader("Location", "/");
  server.send(303);
}

static void enterSignIn();
static void enterRun();

static void setupRoutes() {
  server.on("/", HTTP_GET, [] {
    if (mode == M_AP) pageWifi();
    else if (mode == M_SIGNIN) pageSignIn("");
    else pageStatus();
  });
  server.on("/wifi", HTTP_POST, [] {
    prefs.putString("ssid", server.arg("ssid"));
    prefs.putString("pass", server.arg("pass"));
    sendPage("<div class=card><b>Saved.</b><p>The display is restarting and joining <b>" + htmlEsc(server.arg("ssid")) +
             "</b>. Reconnect this device to your normal WiFi, then follow the screen.</p></div>");
    delay(1500);
    ESP.restart();
  });
  server.on("/code", HTTP_POST, [] {
    String raw = server.arg("code");
    raw.trim();
    int hash = raw.indexOf('#');
    String code = hash >= 0 ? raw.substring(0, hash) : raw;
    String state = hash >= 0 ? raw.substring(hash + 1) : pkceState;
    if (!pkceVerifier.length()) { pageSignIn("Session expired, try again."); return; }
    if (state != pkceState) { pageSignIn("That code is from an older sign-in link. Use the new link below."); return; }
    JsonDocument d;
    d["grant_type"] = "authorization_code";
    d["code"] = code;
    d["state"] = state;
    d["client_id"] = CLIENT_ID;
    d["redirect_uri"] = REDIRECT_URI;
    d["code_verifier"] = pkceVerifier;
    String body, resp;
    serializeJson(d, body);
    int rc = httpsJson("POST", TOKEN_URL, body, resp, false);
    Serial.printf("[auth] exchange -> %d\n", rc);
    if (rc != 200 || !storeTokenResponse(resp)) {
      Serial.println(resp.substring(0, 300));
      pageSignIn("Sign-in failed (" + String(rc) + "). Try again.");
      return;
    }
    pkceVerifier = "";
    sendPage("<div class=card><b class=ok>Signed in.</b><p>Your usage is on the display now. Bookmark this page to check it from anywhere on your network.</p><a class=btn href=/>Open status</a></div>");
    enterRun();
  });
  server.on("/refresh", HTTP_POST, [] { nextFetchAt = millis(); redirectHome(); });
  server.on("/signout", HTTP_POST, [] { signOut(); redirectHome(); enterSignIn(); });
  server.on("/forget", HTTP_POST, [] {
    prefs.clear();
    sendPage("<div class=card>Reset. The display is restarting into setup mode.</div>");
    delay(1000);
    ESP.restart();
  });
  server.onNotFound([] {
    if (mode == M_AP) {  // captive portal
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

static void enterSignIn() {
  mode = M_SIGNIN;
  screenSignIn();
}

static void enterRun() {
  mode = M_RUN;
  lastError = "";
  screenMessage("Loading your usage", "Talking to Claude...");
  if (!plan.length()) fetchPlan();
  fetchUsage();
  if (mode != M_RUN) return;  // sign-in got revoked
  drawMainFrame();
  animateTo();
  drawFooter();
  nextFetchAt = millis() + POLL_MS;
}

static bool connectWifi() {
  screenMessage("Connecting to WiFi", wifiSsid);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);
  if (WiFi.status() != WL_CONNECTED) {
    connectError = "Couldn't join " + wifiSsid;
    return false;
  }
  WiFi.setAutoReconnect(true);
  Serial.printf("[wifi] %s\n", WiFi.localIP().toString().c_str());
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
  t0 = millis();
  while (!timeValid() && millis() - t0 < 10000) delay(100);
  MDNS.begin(HOSTNAME);
  MDNS.addService("http", "tcp", 80);
  return true;
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
  else if (!refreshTok.length()) enterSignIn();
  else enterRun();
  server.begin();
}

void loop() {
  server.handleClient();
  if (mode == M_AP) dns.processNextRequest();

  // BOOT button: tap = refresh, hold 5 s = factory reset
  static uint32_t downAt = 0;
  if (digitalRead(0) == LOW) {
    if (!downAt) downAt = millis();
    if (millis() - downAt > 5000) {
      screenMessage("Reset", "Forgetting WiFi and sign-in");
      prefs.clear();
      delay(1200);
      ESP.restart();
    }
  } else if (downAt) {
    if (millis() - downAt > 50 && mode == M_RUN) nextFetchAt = millis();
    downAt = 0;
  }

  if (mode != M_RUN) return;

  if ((long)(millis() - nextFetchAt) >= 0) {
    nextFetchAt = millis() + POLL_MS;
    tft.fillCircle(18, 308, 4, c565(C_CLAUDE));  // activity dot
    bool ok = fetchUsage();
    if (mode != M_RUN) { enterSignIn(); return; }
    if (ok) {
      drawHeader();
      animateTo();
      for (int i = 0; i < 2; i++) drawCardText(i);
    }
    drawFooter();
  }

  static uint32_t lastTick = 0;
  static int lastMin = -1;
  if (millis() - lastTick > 1000) {
    lastTick = millis();
    time_t n = nowT();
    struct tm lt;
    localtime_r(&n, &lt);
    if (lt.tm_min != lastMin) {
      lastMin = lt.tm_min;
      drawHeader();
      for (int i = 0; i < 2; i++) drawCardText(i);
      drawFooter();
    }
  }
}
