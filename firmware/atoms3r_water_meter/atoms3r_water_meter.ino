// =============================================================================
//  Water meter reader for washing machines - M5Stack AtomS3R
//
//  Reads 3 water meters (Cold, Warm, Hot) from the same magnet contact that
//  drives each Trumeter 7111 counter, shows the totals on screen and sends
//  the readings to Supabase (function submit_reading, see /supabase).
//
//  Set-up at installation: web page ("SETUP" mode, the Atom's own Wi-Fi).
//  - First power-up (no Wi-Fi configured): goes into SETUP by itself.
//  - Afterwards: hold the button (the screen) for 5 s to enter SETUP.
//  - When on Wi-Fi, the page is also at http://<atom-ip>/
//    (user "admin" + the password chosen in the settings).
// =============================================================================
#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <time.h>
#include "supabase_roots.h"

#define FW_VERSION "2.0.0"

// ----------------------------------------------------------------------------
// Supabase project, publishable key and ingest token
// Used whenever the matching field on the settings page is left blank.
// Optional: copy secrets.example.h to secrets.h (never committed) to build the
// key and token into the firmware, so new Atoms only need the Wi-Fi details.
// ----------------------------------------------------------------------------
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef SUPABASE_URL
#define SUPABASE_URL "https://obdltdjykzrxxpozupqt.supabase.co"
#endif
#ifndef SUPABASE_KEY
#define SUPABASE_KEY ""   // publishable key (sb_publishable_...) or legacy anon key
#endif
#ifndef INGEST_TOKEN
#define INGEST_TOKEN ""   // from: select private.create_ingest_token('...');
#endif
static const char *DEFAULT_URL = SUPABASE_URL;
static const char *DEFAULT_KEY = SUPABASE_KEY;
static const char *DEFAULT_TOKEN = INGEST_TOKEN;
static const char *RPC_PATH = "/rest/v1/rpc/submit_reading";

// ----------------------------------------------------------------------------
// Constants
// ----------------------------------------------------------------------------
static const int NUM_CH = 3;
static const char *CH_NAMES[NUM_CH] = {"Cold", "Warm", "Hot"};
static const char *CH_SHORT[NUM_CH] = {"C", "W", "H"};
// Free AtomS3R GPIOs (bottom header + Grove port)
static const uint8_t ALLOWED_PINS[] = {5, 6, 7, 8, 38, 39, 1, 2};
static const uint8_t DEFAULT_PINS[NUM_CH] = {5, 6, 7};

static const uint32_t SAVE_EVERY_MS = 30UL * 1000;      // save counts to flash
static const uint32_t FIRST_SEND_DELAY_MS = 20UL * 1000; // first send after connecting
static const uint32_t CONFIG_LONGPRESS_MS = 5000;
static const uint32_t CONFIG_AP_TIMEOUT_MS = 15UL * 60 * 1000;
static const char *AP_PASSWORD = "watermeter";           // password of the SETUP Wi-Fi

enum InputMode : uint8_t {
  MODE_PARALLEL_7111 = 0, // no pull-up (advanced: an external pull-up holds the level)
  MODE_PULLUP = 1         // ESP internal pull-up: via 1N4148 diode to the 7111, optocoupler or bare sensor
};

// ----------------------------------------------------------------------------
// Settings (stored in flash, namespace "cfg")
// ----------------------------------------------------------------------------
struct ChannelCfg {
  bool enabled;
  uint8_t pin;
  float lpp;     // litres per pulse
  uint8_t mode;  // InputMode
};

struct Config {
  String machine;
  String ssid;
  String pass;
  String url;      // Supabase project URL (https://<ref>.supabase.co)
  String key;      // Supabase publishable / anon key (safe to be public)
  String token;    // ingest token (secret, checked by submit_reading)
  String adminPass;
  String tz;       // POSIX time zone
  uint16_t intervalMin;
  uint16_t debounceMs;
  uint16_t screenOffS;  // screen turns off after this many seconds (0 = always on)
  ChannelCfg ch[NUM_CH];
} cfg;

Preferences prefs;

static void loadConfig() {
  prefs.begin("cfg", true);
  cfg.machine = prefs.getString("mach", "Machine 01");
  cfg.ssid = prefs.getString("ssid", "");
  cfg.pass = prefs.getString("pass", "");
  cfg.url = prefs.getString("url", "");
  cfg.key = prefs.getString("key", "");
  cfg.token = prefs.getString("tok", "");
  // Settings from firmware 1.x pointed at the Google Apps Script: drop them.
  if (cfg.url.indexOf("script.google") >= 0) { cfg.url = ""; cfg.token = ""; }
  if (cfg.url.isEmpty()) cfg.url = DEFAULT_URL;
  if (cfg.key.isEmpty()) cfg.key = DEFAULT_KEY;
  if (cfg.token.isEmpty()) cfg.token = DEFAULT_TOKEN;
  cfg.adminPass = prefs.getString("adm", "watermeter");
  cfg.tz = prefs.getString("tz", "GMT0IST,M3.5.0/1,M10.5.0");
  cfg.intervalMin = prefs.getUShort("int", 60);
  cfg.debounceMs = prefs.getUShort("deb", 30);
  cfg.screenOffS = prefs.getUShort("scr", 60);
  for (int i = 0; i < NUM_CH; i++) {
    char k[8];
    snprintf(k, sizeof k, "c%den", i);  cfg.ch[i].enabled = prefs.getBool(k, true);
    snprintf(k, sizeof k, "c%dpin", i); cfg.ch[i].pin = prefs.getUChar(k, DEFAULT_PINS[i]);
    snprintf(k, sizeof k, "c%dlpp", i); cfg.ch[i].lpp = prefs.getFloat(k, 1.0f);
    snprintf(k, sizeof k, "c%dmod", i); cfg.ch[i].mode = prefs.getUChar(k, MODE_PULLUP);
  }
  prefs.end();
  if (cfg.intervalMin < 1) cfg.intervalMin = 1;
  if (cfg.debounceMs < 2) cfg.debounceMs = 2;
}

static void saveConfig() {
  prefs.begin("cfg", false);
  prefs.putString("mach", cfg.machine);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("url", cfg.url);
  prefs.putString("key", cfg.key);
  prefs.putString("tok", cfg.token);
  prefs.putString("adm", cfg.adminPass);
  prefs.putString("tz", cfg.tz);
  prefs.putUShort("int", cfg.intervalMin);
  prefs.putUShort("deb", cfg.debounceMs);
  prefs.putUShort("scr", cfg.screenOffS);
  for (int i = 0; i < NUM_CH; i++) {
    char k[8];
    snprintf(k, sizeof k, "c%den", i);  prefs.putBool(k, cfg.ch[i].enabled);
    snprintf(k, sizeof k, "c%dpin", i); prefs.putUChar(k, cfg.ch[i].pin);
    snprintf(k, sizeof k, "c%dlpp", i); prefs.putFloat(k, cfg.ch[i].lpp);
    snprintf(k, sizeof k, "c%dmod", i); prefs.putUChar(k, cfg.ch[i].mode);
  }
  prefs.end();
}

static bool pinAllowed(int p) {
  for (uint8_t a : ALLOWED_PINS) if (a == p) return true;
  return false;
}

// ----------------------------------------------------------------------------
// Pulse counting (sampled every 1 ms with a debounce filter)
// ----------------------------------------------------------------------------
struct ChState {
  bool en;
  gpio_num_t pin;
  uint8_t stable;     // current stable level (1 = open, 0 = closed)
  uint16_t run;       // consecutive ms at the new level
  uint64_t pulses;
  uint32_t lastPulseMs;
};

static ChState st[NUM_CH];
static portMUX_TYPE countMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint16_t g_debounce = 30;
static esp_timer_handle_t tickTimer;
static uint64_t savedPulses[NUM_CH];
static uint32_t lastSaveMs = 0;

static void tickCb(void *) {
  uint32_t now = millis();
  for (int i = 0; i < NUM_CH; i++) {
    ChState &c = st[i];
    if (!c.en) continue;
    uint8_t lvl = gpio_get_level(c.pin) ? 1 : 0;
    if (lvl != c.stable) {
      if (++c.run >= g_debounce) {
        c.stable = lvl;
        c.run = 0;
        if (lvl == 0) {               // contact closed = 1 pulse
          portENTER_CRITICAL(&countMux);
          c.pulses++;
          portEXIT_CRITICAL(&countMux);
          c.lastPulseMs = now;
        }
      }
    } else {
      c.run = 0;
    }
  }
}

static uint64_t getPulses(int i) {
  portENTER_CRITICAL(&countMux);
  uint64_t v = st[i].pulses;
  portEXIT_CRITICAL(&countMux);
  return v;
}

static void setPulses(int i, uint64_t v) {
  portENTER_CRITICAL(&countMux);
  st[i].pulses = v;
  portEXIT_CRITICAL(&countMux);
}

static double litres(int i) { return (double)getPulses(i) * (double)cfg.ch[i].lpp; }

static void loadCounts() {
  prefs.begin("cnt", true);
  uint64_t v[NUM_CH] = {0, 0, 0};
  if (prefs.getBytesLength("p") == sizeof v) prefs.getBytes("p", v, sizeof v);
  prefs.end();
  for (int i = 0; i < NUM_CH; i++) { st[i].pulses = v[i]; savedPulses[i] = v[i]; }
}

static void saveCounts(bool force) {
  uint64_t v[NUM_CH];
  bool changed = false;
  for (int i = 0; i < NUM_CH; i++) {
    v[i] = getPulses(i);
    if (v[i] != savedPulses[i]) changed = true;
  }
  if (!changed && !force) return;
  prefs.begin("cnt", false);
  prefs.putBytes("p", v, sizeof v);
  prefs.end();
  for (int i = 0; i < NUM_CH; i++) savedPulses[i] = v[i];
}

static void setupCounting() {
  g_debounce = cfg.debounceMs;
  for (int i = 0; i < NUM_CH; i++) {
    ChState &c = st[i];
    c.en = cfg.ch[i].enabled && pinAllowed(cfg.ch[i].pin);
    c.pin = (gpio_num_t)cfg.ch[i].pin;
    c.run = 0;
    c.lastPulseMs = 0;
    if (!c.en) continue;
    pinMode(c.pin, cfg.ch[i].mode == MODE_PULLUP ? INPUT_PULLUP : INPUT);
    delay(5);
    c.stable = gpio_get_level(c.pin) ? 1 : 0;  // initial state is not counted
  }
  esp_timer_create_args_t args = {};
  args.callback = &tickCb;
  args.name = "pulses";
  esp_timer_create(&args, &tickTimer);
  esp_timer_start_periodic(tickTimer, 1000);  // 1 ms
}

// ----------------------------------------------------------------------------
// Network / sending state
// ----------------------------------------------------------------------------
static bool configMode = false;
static uint32_t configStartMs = 0;
static WebServer server(80);
static DNSServer dns;
static String apName;

static volatile bool sendRequested = false;
static String lastSendMsg = "waiting";
static time_t lastSendOkAt = 0;
static bool lastSendOk = false;
static uint32_t sendSeq = 0;
static uint32_t bootId = 0;

static String twoDigits(int v) { return (v < 10 ? "0" : "") + String(v); }

static String clockStr(time_t t) {
  if (t < 1600000000) return "--:--";
  struct tm tmv;
  localtime_r(&t, &tmv);
  return twoDigits(tmv.tm_hour) + ":" + twoDigits(tmv.tm_min);
}

static bool doSend() {
  if (cfg.url.isEmpty()) { lastSendMsg = "no URL"; return false; }
  if (cfg.key.isEmpty()) { lastSendMsg = "no API key"; return false; }
  if (cfg.token.isEmpty()) { lastSendMsg = "no token"; return false; }

  JsonDocument doc;
  JsonObject p = doc["payload"].to<JsonObject>();
  p["token"] = cfg.token;
  p["machine"] = cfg.machine;
  p["device"] = WiFi.macAddress();
  p["fw"] = FW_VERSION;
  p["boot"] = bootId;
  p["seq"] = ++sendSeq;
  p["uptime_s"] = (uint32_t)(millis() / 1000);
  p["rssi"] = WiFi.RSSI();
  JsonArray arr = p["channels"].to<JsonArray>();
  for (int i = 0; i < NUM_CH; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = CH_NAMES[i];
    o["enabled"] = st[i].en;
    o["pulses"] = (unsigned long long)getPulses(i);
    o["lpp"] = cfg.ch[i].lpp;
    o["litres"] = litres(i);
  }
  String body;
  serializeJson(doc, body);

  String url = cfg.url;
  while (url.endsWith("/")) url.remove(url.length() - 1);
  url += RPC_PATH;

  WiFiClientSecure client;
  client.setCACert(SUPABASE_ROOT_CAS);
  HTTPClient http;
  http.setTimeout(20000);
  if (!http.begin(client, url)) { lastSendMsg = "bad URL"; return false; }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", cfg.key);
  // Legacy anon keys are JWTs and also go in Authorization. New sb_publishable_
  // keys must only go in the apikey header.
  if (cfg.key.startsWith("eyJ")) http.addHeader("Authorization", "Bearer " + cfg.key);
  int code = http.POST(body);
  String resp = code > 0 ? http.getString() : String();
  http.end();

  bool ok = code == 200 && (resp.indexOf("\"ok\":true") >= 0 || resp.indexOf("\"ok\": true") >= 0);
  if (ok) {
    lastSendOk = true;
    lastSendOkAt = time(nullptr);
    lastSendMsg = "sent " + clockStr(lastSendOkAt);
  } else {
    lastSendOk = false;
    if (resp.indexOf("\"token\"") >= 0) lastSendMsg = "wrong token";
    else if (code == 401) lastSendMsg = "wrong API key";
    else if (code == 404) lastSendMsg = "run SQL setup";
    else lastSendMsg = "error " + String(code);
    Serial.printf("Send failed: HTTP %d %s\n", code, resp.substring(0, 200).c_str());
  }
  return ok;
}

static void sendTask(void *) {
  uint32_t connectedAt = 0;
  uint32_t lastAttempt = 0;
  bool firstDone = false;
  for (;;) {
    if (!configMode && WiFi.status() == WL_CONNECTED) {
      if (connectedAt == 0) connectedAt = millis();
      uint32_t interval = (uint32_t)cfg.intervalMin * 60000UL;
      bool due = (!firstDone && millis() - connectedAt >= FIRST_SEND_DELAY_MS) ||
                 (firstDone && millis() - lastAttempt >= interval);
      if (due || sendRequested) {
        sendRequested = false;
        firstDone = true;
        lastAttempt = millis();
        doSend();
      }
    } else {
      connectedAt = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// ----------------------------------------------------------------------------
// Settings page
// ----------------------------------------------------------------------------
static String esc(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\'': o += "&#39;"; break;
      default: o += c;
    }
  }
  return o;
}

static bool checkAuth() {
  if (configMode) return true;  // on the SETUP Wi-Fi, its password already protects the page
  if (!server.authenticate("admin", cfg.adminPass.c_str())) {
    server.requestAuthentication(BASIC_AUTH, "Water meter");
    return false;
  }
  return true;
}

static const char PAGE_HEAD[] PROGMEM = R"HTML(<!doctype html><html lang="en-IE"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Water meter</title><style>
body{font-family:system-ui,sans-serif;margin:0;padding:16px;background:#f4f6f8;color:#1b1f24;max-width:640px}
h1{font-size:20px;margin:0 0 4px}h2{font-size:16px;margin:20px 0 8px}
fieldset{border:1px solid #cdd3da;border-radius:8px;margin:0 0 12px;padding:10px 12px;background:#fff}
legend{font-weight:600;padding:0 4px}label{display:block;font-size:13px;margin:8px 0 2px;color:#4a5560}
input,select{width:100%;box-sizing:border-box;padding:8px;font-size:15px;border:1px solid #b9c1ca;border-radius:6px}
input[type=checkbox]{width:auto}button{padding:10px 16px;font-size:15px;border:0;border-radius:6px;background:#0b6bcb;color:#fff;margin-top:8px}
.muted{color:#6a7580;font-size:13px}.row{display:flex;gap:8px}.row>div{flex:1}
table{border-collapse:collapse;width:100%;background:#fff}td,th{border:1px solid #dde2e7;padding:6px;font-size:14px;text-align:left}
.warn{background:#fff4d6;border:1px solid #f0c94a;padding:8px;border-radius:6px;font-size:13px}
</style></head><body>)HTML";

static void handleRoot() {
  if (!checkAuth()) return;
  String h;
  h.reserve(9000);
  h += FPSTR(PAGE_HEAD);
  h += "<h1>Water meter &middot; " + esc(cfg.machine) + "</h1>";
  h += "<div class='muted'>Firmware " FW_VERSION " &middot; " + WiFi.macAddress() + "</div>";

  // Current status
  h += "<h2>Current readings</h2><table><tr><th>Inlet</th><th>Pulses</th><th>Litres</th></tr>";
  for (int i = 0; i < NUM_CH; i++) {
    h += "<tr><td>" + String(CH_NAMES[i]) + (st[i].en ? "" : " (off)") + "</td><td>" +
         String((unsigned long long)getPulses(i)) + "</td><td>" + String(litres(i), 1) + "</td></tr>";
  }
  h += "</table><div class='muted'>Wi-Fi: " +
       (WiFi.status() == WL_CONNECTED ? esc(WiFi.SSID()) + " (" + WiFi.localIP().toString() + ", " + String(WiFi.RSSI()) + " dBm)" : String("not connected")) +
       " &middot; last send: " + esc(lastSendMsg) + "</div>";

  // Main form
  h += "<form method='post' action='/save'>";
  h += "<h2>Machine and network</h2><fieldset><legend>General</legend>";
  h += "<label>Machine name (shown on the web page)</label><input name='mach' maxlength='32' value='" + esc(cfg.machine) + "'>";
  h += "<label>Wi-Fi network</label><input name='ssid' list='nets' value='" + esc(cfg.ssid) + "'><datalist id='nets'>";
  int n = WiFi.scanComplete();
  if (n > 0) for (int i = 0; i < n; i++) h += "<option value='" + esc(WiFi.SSID(i)) + "'>";
  h += "</datalist>";
  h += "<label>Wi-Fi password (leave blank to keep)</label><input name='pass' type='password'>";
  h += "<label>Supabase project URL</label><input name='url' value='" + esc(cfg.url) + "'>";
  h += "<label>Supabase publishable key (sb_publishable_...)</label><input name='key' value='" + esc(cfg.key) + "'>";
  h += "<label>Ingest token (blank = keep)</label><input name='tok' type='password' placeholder='" + String(cfg.token.length() ? "saved" : "not set") + "'>";
  h += "<div class='row'><div><label>Send every (min)</label><input name='int' type='number' min='1' max='1440' value='" + String(cfg.intervalMin) + "'></div>";
  h += "<div><label>Debounce filter (ms)</label><input name='deb' type='number' min='2' max='500' value='" + String(cfg.debounceMs) + "'></div></div>";
  h += "<label>Screen off after (seconds, 0 = always on)</label><input name='scr' type='number' min='0' max='3600' value='" + String(cfg.screenOffS) + "'>";
  h += "<label>Time zone (POSIX)</label><input name='tz' value='" + esc(cfg.tz) + "'>";
  h += "<div class='muted'>Ireland: GMT0IST,M3.5.0/1,M10.5.0</div>";
  h += "<label>Password for this page, user admin (blank = keep)</label><input name='adm' type='password'>";
  h += "</fieldset>";

  h += "<h2>Water inlets</h2>";
  for (int i = 0; i < NUM_CH; i++) {
    String p = "c" + String(i);
    h += "<fieldset><legend>" + String(CH_NAMES[i]) + "</legend>";
    h += "<label><input type='checkbox' name='" + p + "en' value='1'" + (cfg.ch[i].enabled ? " checked" : "") + "> Inlet in use</label>";
    h += "<div class='row'><div><label>Pin</label><select name='" + p + "pin'>";
    for (uint8_t a : ALLOWED_PINS) {
      h += "<option value='" + String(a) + "'" + (cfg.ch[i].pin == a ? " selected" : "") + ">G" + String(a) + "</option>";
    }
    h += "</select></div><div><label>Litres per pulse</label><input name='" + p + "lpp' type='number' step='any' min='0.0001' value='" + String(cfg.ch[i].lpp, 4) + "'></div></div>";
    h += "<label>Wiring</label><select name='" + p + "mod'>";
    h += String("<option value='0'") + (cfg.ch[i].mode == MODE_PARALLEL_7111 ? " selected" : "") + ">No pull-up (advanced)</option>";
    h += String("<option value='1'") + (cfg.ch[i].mode == MODE_PULLUP ? " selected" : "") + ">Internal pull-up: diode to the 7111, optocoupler or bare sensor</option>";
    h += "</select></fieldset>";
  }
  h += "<button type='submit'>Save and restart</button></form>";

  // Count adjustment
  h += "<h2>Match the count to the 7111</h2><form method='post' action='/counts'><fieldset><legend>Current pulses</legend>";
  h += "<div class='muted'>Type the number shown on each 7111 counter. Blank = leave as is.</div>";
  for (int i = 0; i < NUM_CH; i++) {
    h += "<label>" + String(CH_NAMES[i]) + " (now: " + String((unsigned long long)getPulses(i)) + ")</label><input name='p" + String(i) + "' type='number' min='0' step='1'>";
  }
  h += "</fieldset><button type='submit'>Save count</button></form>";

  h += "<h2>Test</h2><form method='post' action='/send'><button type='submit'>Send a reading now</button></form>";
  if (configMode) h += "<form method='post' action='/exit'><button type='submit' style='background:#55606b'>Leave SETUP mode</button></form>";
  h += "</body></html>";
  server.send(200, "text/html; charset=utf-8", h);
}

static void sendMessagePage(const String &title, const String &msg, bool back) {
  String h = FPSTR(PAGE_HEAD);
  h += "<h1>" + title + "</h1><p>" + msg + "</p>";
  if (back) h += "<p><a href='/'>Back</a></p>";
  h += "</body></html>";
  server.send(200, "text/html; charset=utf-8", h);
}

static void handleSave() {
  if (!checkAuth()) return;
  Config n = cfg;
  n.machine = server.arg("mach"); n.machine.trim();
  if (n.machine.isEmpty()) n.machine = "Machine";
  n.ssid = server.arg("ssid"); n.ssid.trim();
  if (server.arg("pass").length()) n.pass = server.arg("pass");
  n.url = server.arg("url"); n.url.trim();
  n.key = server.arg("key"); n.key.trim();
  if (server.arg("tok").length()) { n.token = server.arg("tok"); n.token.trim(); }
  n.intervalMin = constrain(server.arg("int").toInt(), 1, 1440);
  n.debounceMs = constrain(server.arg("deb").toInt(), 2, 500);
  n.screenOffS = constrain(server.arg("scr").toInt(), 0, 3600);
  n.tz = server.arg("tz"); n.tz.trim();
  if (n.tz.isEmpty()) n.tz = "GMT0";
  if (server.arg("adm").length()) n.adminPass = server.arg("adm");

  String err;
  for (int i = 0; i < NUM_CH; i++) {
    String p = "c" + String(i);
    n.ch[i].enabled = server.hasArg(p + "en");
    n.ch[i].pin = (uint8_t)server.arg(p + "pin").toInt();
    n.ch[i].lpp = server.arg(p + "lpp").toFloat();
    n.ch[i].mode = server.arg(p + "mod").toInt() == 1 ? MODE_PULLUP : MODE_PARALLEL_7111;
    if (!pinAllowed(n.ch[i].pin)) err += "Invalid pin for " + String(CH_NAMES[i]) + ". ";
    if (n.ch[i].lpp <= 0) err += "Invalid litres per pulse for " + String(CH_NAMES[i]) + ". ";
  }
  for (int i = 0; i < NUM_CH; i++)
    for (int j = i + 1; j < NUM_CH; j++)
      if (n.ch[i].enabled && n.ch[j].enabled && n.ch[i].pin == n.ch[j].pin)
        err += String(CH_NAMES[i]) + " and " + CH_NAMES[j] + " are on the same pin. ";
  if (err.length()) { sendMessagePage("Nothing was saved", esc(err), true); return; }

  cfg = n;
  saveConfig();
  saveCounts(true);
  prefs.begin("cfg", false); prefs.putBool("forceAP", false); prefs.end();
  sendMessagePage("Saved", "Restarting. If the Wi-Fi details are right, the screen will show the readings and the IP address.", false);
  delay(1500);
  ESP.restart();
}

static void handleCounts() {
  if (!checkAuth()) return;
  for (int i = 0; i < NUM_CH; i++) {
    String v = server.arg("p" + String(i));
    v.trim();
    if (v.length()) setPulses(i, strtoull(v.c_str(), nullptr, 10));
  }
  saveCounts(true);
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handleSendNow() {
  if (!checkAuth()) return;
  if (configMode) { sendMessagePage("SETUP mode", "To send, save the settings first (the Atom joins the Wi-Fi after restarting).", true); return; }
  sendRequested = true;
  sendMessagePage("Sending", "Send requested. Check the result on the Atom screen, or refresh the main page in a few seconds.", true);
}

static void handleExit() {
  saveCounts(true);
  prefs.begin("cfg", false); prefs.putBool("forceAP", false); prefs.end();
  sendMessagePage("Leaving", "Restarting in normal mode.", false);
  delay(1000);
  ESP.restart();
}

static void setupServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/counts", HTTP_POST, handleCounts);
  server.on("/send", HTTP_POST, handleSendNow);
  server.on("/exit", HTTP_POST, handleExit);
  server.onNotFound([]() {
    if (configMode) {  // captive portal: any address opens the settings
      server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
      server.send(302);
    } else {
      server.send(404, "text/plain", "Not found");
    }
  });
  server.begin();
}

// ----------------------------------------------------------------------------
// 128x128 screen
// ----------------------------------------------------------------------------
static M5Canvas canvas(&M5.Display);
static bool showPulses = false;
static uint16_t chColor[NUM_CH];

static String fmtLitres(double l) {
  if (l < 100000.0) return String((unsigned long)(l + 0.5)) + " L";
  return String(l / 1000.0, 1) + " m3";
}

static void drawScreen() {
  canvas.fillScreen(TFT_BLACK);
  canvas.setTextDatum(TL_DATUM);

  // Header: machine name
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(TFT_WHITE);
  String title = cfg.machine;
  while (title.length() > 1 && canvas.textWidth(title) > 124) title.remove(title.length() - 1);
  canvas.drawString(title, 2, 0);
  canvas.drawFastHLine(0, 17, 128, 0x4208);

  if (configMode) {
    canvas.setTextColor(TFT_YELLOW);
    canvas.drawString("SETUP MODE", 2, 22);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString("Wi-Fi:", 2, 44);
    canvas.drawString(apName, 2, 56);
    canvas.drawString("Password:", 2, 72);
    canvas.drawString(AP_PASSWORD, 2, 84);
    canvas.drawString("Open 192.168.4.1", 2, 100);
    canvas.drawString("Still counting", 2, 116);
    canvas.pushSprite(0, 0);
    return;
  }

  // 3 rows: Cold / Warm / Hot
  for (int i = 0; i < NUM_CH; i++) {
    int y = 21 + i * 30;
    canvas.fillRoundRect(0, y, 18, 26, 3, st[i].en ? chColor[i] : 0x4208);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(TFT_BLACK);
    canvas.setTextDatum(MC_DATUM);
    canvas.drawString(CH_SHORT[i], 9, y + 13);
    canvas.setTextDatum(MR_DATUM);
    if (!st[i].en) {
      canvas.setTextColor(0x8410);
      canvas.drawString("off", 126, y + 13);
      continue;
    }
    String v = showPulses ? String((unsigned long long)getPulses(i)) : fmtLitres(litres(i));
    canvas.setFont(&fonts::Font4);
    if (canvas.textWidth(v) > 104) canvas.setFont(&fonts::Font2);
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString(v, 126, y + 14);
    // dot flashes when a pulse comes in
    if (millis() - st[i].lastPulseMs < 300 && st[i].lastPulseMs != 0) canvas.fillCircle(22, y + 4, 2, chColor[i]);
  }
  canvas.setTextDatum(TL_DATUM);

  // Footer: network and sending
  canvas.drawFastHLine(0, 112, 128, 0x4208);
  canvas.setFont(&fonts::Font0);
  String foot;
  uint16_t col = TFT_WHITE;
  if (WiFi.status() != WL_CONNECTED) { foot = "no Wi-Fi"; col = TFT_ORANGE; }
  else { foot = lastSendMsg; col = lastSendOk ? TFT_GREENYELLOW : (lastSendMsg == "waiting" ? TFT_WHITE : TFT_ORANGE); }
  canvas.setTextColor(col);
  canvas.drawString(foot, 2, 117);
  canvas.setTextColor(0x8410);
  canvas.setTextDatum(TR_DATUM);
  canvas.drawString(showPulses ? "pulses" : clockStr(time(nullptr)), 126, 117);
  canvas.setTextDatum(TL_DATUM);
  canvas.pushSprite(0, 0);
}

static void drawIpBriefly() {
  canvas.fillScreen(TFT_BLACK);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString("Connected", 2, 30);
  canvas.drawString(WiFi.localIP().toString(), 2, 52);
  canvas.setFont(&fonts::Font0);
  canvas.drawString("config: http://<ip>/", 2, 80);
  canvas.pushSprite(0, 0);
}

// ----------------------------------------------------------------------------
// Wi-Fi
// ----------------------------------------------------------------------------
static void startConfigMode() {
  configMode = true;
  configStartMs = millis();
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[24];
  snprintf(buf, sizeof buf, "WaterMeter-%02X%02X", mac[4], mac[5]);
  apName = buf;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apName.c_str(), AP_PASSWORD);
  dns.start(53, "*", WiFi.softAPIP());
  WiFi.scanNetworks(true);  // network list for the page
  Serial.printf("SETUP mode: Wi-Fi %s password %s -> http://192.168.4.1/\n", apName.c_str(), AP_PASSWORD);
}

static void startStation() {
  String host = "watermeter-" + cfg.machine;
  host.replace(" ", "-");
  host.toLowerCase();
  host = host.substring(0, 31);
  WiFi.setHostname(host.c_str());  // before WiFi.mode()
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  configTzTime(cfg.tz.c_str(), "pool.ntp.org", "time.google.com");
}

// ----------------------------------------------------------------------------
// setup / loop
// ----------------------------------------------------------------------------
void setup() {
  auto mcfg = M5.config();
  M5.begin(mcfg);
  Serial.begin(115200);
  M5.Display.setRotation(0);
  M5.Display.setBrightness(80);
  canvas.setColorDepth(16);
  canvas.createSprite(128, 128);
  chColor[0] = M5.Display.color565(70, 160, 255);   // Cold
  chColor[1] = M5.Display.color565(255, 190, 60);   // Warm
  chColor[2] = M5.Display.color565(255, 80, 70);    // Hot

  bootId = esp_random();
  loadConfig();
  loadCounts();
  setupCounting();

  prefs.begin("cfg", true);
  bool forceAP = prefs.getBool("forceAP", false);
  prefs.end();

  // Button held at power-up = SETUP
  M5.update();
  bool btnAtBoot = M5.BtnA.isPressed();

  if (cfg.ssid.isEmpty() || forceAP || btnAtBoot) startConfigMode();
  else startStation();

  setupServer();
  xTaskCreatePinnedToCore(sendTask, "sender", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  M5.update();
  if (configMode) dns.processNextRequest();
  server.handleClient();

  // Screen timeout: any press wakes the screen
  static uint32_t lastActivity = 0;
  static bool screenOn = true;
  static bool wakePress = false;  // the press that woke the screen does not toggle
  if (M5.BtnA.wasPressed()) {
    if (!screenOn) {
      screenOn = true;
      wakePress = true;
      M5.Display.wakeup();
      M5.Display.setBrightness(80);
    }
    lastActivity = millis();
  }
  if (screenOn && !configMode && cfg.screenOffS > 0 && !M5.BtnA.isPressed() &&
      millis() - lastActivity > (uint32_t)cfg.screenOffS * 1000UL) {
    screenOn = false;
    M5.Display.setBrightness(0);
    M5.Display.sleep();
  }

  // Button: tap = litres/pulses toggle; 5 s = SETUP mode
  static bool longHandled = false;
  if (M5.BtnA.pressedFor(CONFIG_LONGPRESS_MS) && !longHandled) {
    longHandled = true;
    if (!configMode) {
      saveCounts(true);
      prefs.begin("cfg", false); prefs.putBool("forceAP", true); prefs.end();
      ESP.restart();
    }
  }
  if (M5.BtnA.wasReleased()) {
    if (!longHandled && !wakePress) showPulses = !showPulses;
    longHandled = false;
    wakePress = false;
  }

  // Leave SETUP by itself after a while (if Wi-Fi is already configured)
  if (configMode && !cfg.ssid.isEmpty() && millis() - configStartMs > CONFIG_AP_TIMEOUT_MS) {
    saveCounts(true);
    prefs.begin("cfg", false); prefs.putBool("forceAP", false); prefs.end();
    ESP.restart();
  }

  // Show the IP for 5 s after connecting
  static bool wasConnected = false;
  static uint32_t ipShownAt = 0;
  bool conn = WiFi.status() == WL_CONNECTED;
  if (conn && !wasConnected && !configMode) { ipShownAt = millis(); Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str()); }
  wasConnected = conn;

  static uint32_t lastDraw = 0;
  if (screenOn && millis() - lastDraw >= 200) {
    lastDraw = millis();
    if (ipShownAt && millis() - ipShownAt < 5000) drawIpBriefly();
    else drawScreen();
  }

  if (millis() - lastSaveMs >= SAVE_EVERY_MS) {
    lastSaveMs = millis();
    saveCounts(false);
  }
  delay(2);
}
