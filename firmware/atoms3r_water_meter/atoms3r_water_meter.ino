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
//
//  By default the Atom is only the gateway (no meters wired to it): the
//  screen lists the readers. Tick "This Atom also reads a machine" on the
//  settings page to count its own 3 inlets as well.
//
//  Screen, per inlet: litres used this week (Monday to Sunday, from
//  Supabase, the same numbers as the web page), the total litres and the
//  counter (pulses, the number to compare with the 7111).
//
//  Gateway: the Atom listens for the battery meter readers (XIAO
//  ESP32-C6) over ESP-NOW, answers them and forwards their readings to
//  Supabase. The readers find the Atom's Wi-Fi channel by themselves; both
//  sides need the same "Network key".
//  Readers with a "machine running" input also send each start / stop; the
//  Atom forwards them with the time they happened (the web page shows the
//  periods) and marks a running machine with a green dot on the screen.
//  Mains powered readers say "still here" every minute; when one goes silent
//  for 2.5 min the Atom reports its machine as switched off.
//  Readers with the older firmware (link version 1) still work.
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
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#include <freertos/queue.h>
#include "supabase_roots.h"

#define FW_VERSION "2.4.0"

// ----------------------------------------------------------------------------
// Supabase project, publishable key and ingest token
// Used whenever the matching field on the settings page is left blank.
// Optional: copy secrets.example.h to secrets.h (never committed) to build the
// key and token into the firmware, so new Atoms only need the Wi-Fi details.
// ----------------------------------------------------------------------------
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#include "espnow_link.h"   // after secrets.h, which may set MESH_KEY
#ifndef SUPABASE_URL
#define SUPABASE_URL "https://obdltdjykzrxxpozupqt.supabase.co"
#endif
#ifndef SUPABASE_KEY
#define SUPABASE_KEY "sb_publishable_BQwFl3_6XV9igdkYUJTFbA_UHM5OhOZ"   // publishable key (public by design)
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
  bool gateway;         // receive readings from the XIAO readers (ESP-NOW)
  bool ownMeter;        // this Atom also reads a machine (its own 3 inlets)
  String meshKey;       // ESP-NOW network key, same as on the readers
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
  cfg.gateway = prefs.getBool("gw", true);
  cfg.ownMeter = prefs.getBool("own", false);
  if (!cfg.ownMeter) cfg.gateway = true;  // nothing else to do
  cfg.meshKey = prefs.getString("mkey", "");
  if (cfg.meshKey.isEmpty()) cfg.meshKey = MESH_KEY;
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
  prefs.putBool("gw", cfg.gateway);
  prefs.putBool("own", cfg.ownMeter);
  prefs.putString("mkey", cfg.meshKey);
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
    c.en = cfg.ownMeter && cfg.ch[i].enabled && pinAllowed(cfg.ch[i].pin);
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

// This week's usage is read back from Supabase after uploads (see "This week" below)
static volatile bool weekDirty = true;     // fetch again soon
static volatile uint32_t weekDirtyMs = 0;  // when it became dirty

static String twoDigits(int v) { return (v < 10 ? "0" : "") + String(v); }

static String clockStr(time_t t) {
  if (t < 1600000000) return "--:--";
  struct tm tmv;
  localtime_r(&t, &tmv);
  return twoDigits(tmv.tm_hour) + ":" + twoDigits(tmv.tm_min);
}

// POST one {"payload": {...}} to submit_reading. msg gets a short result.
static bool postReading(JsonDocument &doc, String &msg) {
  if (cfg.url.isEmpty()) { msg = "no URL"; return false; }
  if (cfg.key.isEmpty()) { msg = "no API key"; return false; }
  if (cfg.token.isEmpty()) { msg = "no token"; return false; }
  doc["payload"]["token"] = cfg.token;
  String body;
  serializeJson(doc, body);

  String url = cfg.url;
  while (url.endsWith("/")) url.remove(url.length() - 1);
  url += RPC_PATH;

  WiFiClientSecure client;
  client.setCACert(SUPABASE_ROOT_CAS);
  HTTPClient http;
  http.setTimeout(20000);
  if (!http.begin(client, url)) { msg = "bad URL"; return false; }
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
    msg = "sent " + clockStr(time(nullptr));
    weekDirty = true;
    weekDirtyMs = millis();
  } else {
    if (resp.indexOf("\"token\"") >= 0) msg = "wrong token";
    else if (code == 401) msg = "wrong API key";
    else if (code == 404) msg = "run SQL setup";
    else msg = "error " + String(code);
    Serial.printf("Send failed: HTTP %d %s\n", code, resp.substring(0, 200).c_str());
  }
  return ok;
}

static bool doSend() {
  JsonDocument doc;
  JsonObject p = doc["payload"].to<JsonObject>();
  p["token"] = cfg.token;
  p["machine"] = cfg.machine;
  p["kind"] = "washer";  // washer | boiler | canteen | other (used when the device is first seen)
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
  bool ok = postReading(doc, lastSendMsg);
  lastSendOk = ok;
  if (ok) lastSendOkAt = time(nullptr);
  return ok;
}

// ----------------------------------------------------------------------------
// Gateway: readings from the XIAO readers over ESP-NOW
// ----------------------------------------------------------------------------
struct NodeInfo {
  bool used;
  uint8_t mac[6];
  char machine[33];
  uint32_t boot, seq;
  uint32_t lastSeenMs;
  int8_t rssi;
  uint16_t battMv;
  uint8_t run;     // LINK_RUN_* / LINK_KEEPALIVE flags of the last packet
  bool offReported;  // reported to Supabase as switched off (silent), waiting for it to come back
  uint16_t offK;     // switched-off reports for this boot (gives the event numbers -1, -2, -3...)
  uint32_t offTryMs; // last attempt to report it
  uint8_t nch;
  uint8_t enabled[LINK_MAX_CH];
  char chName[LINK_MAX_CH][9];
  uint64_t pulses[LINK_MAX_CH];
  float lpp[LINK_MAX_CH];
  char status[24];
};

static const int MAX_NODES = 24;
static NodeInfo nodes[MAX_NODES];
static portMUX_TYPE nodesMux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t rxQueue = nullptr;   // radio -> gateway task
static QueueHandle_t upQueue = nullptr;   // gateway task -> sender task
static bool gatewayRunning = false;
static uint32_t gwReceived = 0, gwBadKey = 0;  // only written by the gateway task

static String macStr(const uint8_t *m) {
  char b[18];
  snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return b;
}

#if ESP_IDF_VERSION_MAJOR >= 5
static void onLinkRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  const uint8_t *mac = info->src_addr;
  int8_t rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
#else
static void onLinkRecv(const uint8_t *mac, const uint8_t *data, int len) {
  int8_t rssi = 0;
#endif
  if (!rxQueue || len < 4) return;
  uint8_t ver = data[2];
  bool v2 = ver == LINK_VERSION && len == (int)sizeof(LinkReading);
  bool v1 = ver == LINK_VERSION_V1 && len == (int)sizeof(LinkReadingV1);
  if (!v1 && !v2) return;
  RxItem it;
  memset(&it, 0, sizeof it);
  memcpy(it.mac, mac, 6);
  it.rssi = rssi;
  it.ver = ver;
  it.rxMs = millis();
  if (v2) {
    memcpy(&it.r, data, sizeof it.r);
  } else {
    // Older reader: same fields up to the tag, no running input
    memcpy(&it.r, data, offsetof(LinkReadingV1, tag));
    memcpy(&it.r.tag, data + offsetof(LinkReadingV1, tag), sizeof it.r.tag);
  }
  if (it.r.magic != LINK_MAGIC || it.r.type != LINK_READING) return;
  // The tag covers the packet as it was sent (so check it before the copy is changed)
  it.tagOk = it.r.tag == link_tag(cfg.meshKey.c_str(), data, len);
  if (it.r.nev > LINK_MAX_EV) it.r.nev = LINK_MAX_EV;
  xQueueSend(rxQueue, &it, 0);
}

static void sendAck(const RxItem &it) {
  LinkAck a;
  memset(&a, 0, sizeof a);
  a.magic = LINK_MAGIC;
  a.version = it.ver;  // older readers only accept an answer of their own version
  a.type = LINK_ACK;
  a.boot = it.r.boot;
  a.seq = it.r.seq;
  a.channel = (uint8_t)WiFi.channel();
  a.rssi = it.rssi;
  memcpy(a.dest, it.mac, 6);
  a.tag = link_tag(cfg.meshKey.c_str(), &a, sizeof a);
  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof peer);
  memcpy(peer.peer_addr, it.mac, 6);
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  bool added = esp_now_add_peer(&peer) == ESP_OK;
  esp_now_send(it.mac, (const uint8_t *)&a, sizeof a);
  if (added) {
    delay(5);
    esp_now_del_peer(it.mac);
  }
}

static void gatewayTask(void *) {
  RxItem it;
  for (;;) {
    if (xQueueReceive(rxQueue, &it, portMAX_DELAY) != pdTRUE) continue;
    if (!it.tagOk) { gwBadKey++; continue; }
    it.r.machine[sizeof it.r.machine - 1] = 0;
    it.r.kind[sizeof it.r.kind - 1] = 0;
    it.r.fw[sizeof it.r.fw - 1] = 0;
    if (it.r.nch > LINK_MAX_CH) it.r.nch = LINK_MAX_CH;
    sendAck(it);  // answer first: the reader waits only ~120 ms
    gwReceived++;

    bool isNew = true;
    portENTER_CRITICAL(&nodesMux);
    int idx = -1, freeIdx = -1, oldest = 0;
    for (int i = 0; i < MAX_NODES; i++) {
      if (nodes[i].used && memcmp(nodes[i].mac, it.mac, 6) == 0) { idx = i; break; }
      if (!nodes[i].used && freeIdx < 0) freeIdx = i;
      if (nodes[i].lastSeenMs < nodes[oldest].lastSeenMs) oldest = i;
    }
    if (idx < 0) { idx = freeIdx >= 0 ? freeIdx : oldest; memset(&nodes[idx], 0, sizeof nodes[idx]); }
    NodeInfo &n = nodes[idx];
    // The same reading again (our answer got lost): answer, but do not upload twice
    if (n.used && n.boot == it.r.boot && it.r.seq <= n.seq) isNew = false;
    // Machine switched off? A mains powered reader that restarted was off since we last heard it
    // (unless that silence was already reported); one that comes back with no restart is "back".
    it.offN = it.backN = 0;
    if (isNew && n.used && (n.run & LINK_KEEPALIVE)) {
      if (n.boot != it.r.boot) {
        if (!n.offReported) {
          it.offN = -1;  // first event number of the new boot (the reader's own numbers start at 1)
          it.offAgoS = (it.rxMs - n.lastSeenMs) / 1000;
        }
        n.offReported = false;
        n.offK = it.offN ? 1 : 0;
      } else if (n.offReported) {
        it.backN = -(int32_t)(2 * n.offK);
        n.offReported = false;
      }
    } else if (n.used && n.boot != it.r.boot) {
      n.offReported = false;
      n.offK = 0;
    }
    n.used = true;
    memcpy(n.mac, it.mac, 6);
    strncpy(n.machine, it.r.machine, sizeof n.machine);
    n.boot = it.r.boot;
    if (isNew) n.seq = it.r.seq;
    n.lastSeenMs = millis();
    n.rssi = it.rssi;
    n.battMv = it.r.battery_mv;
    n.run = it.r.run;
    n.nch = it.r.nch;
    for (int c = 0; c < n.nch; c++) {
      n.enabled[c] = it.r.ch[c].enabled;
      memcpy(n.chName[c], it.r.ch[c].name, 8);
      n.chName[c][8] = 0;
      n.pulses[c] = it.r.ch[c].pulses;
      n.lpp[c] = it.r.ch[c].lpp;
    }
    // A "still here" is only uploaded when it carries changes
    bool upload = isNew && (!(it.r.run & LINK_ALIVE_ONLY) || it.r.nev || it.offN || it.backN);
    if (upload) strncpy(n.status, "queued", sizeof n.status);
    portEXIT_CRITICAL(&nodesMux);

    if (upload && xQueueSend(upQueue, &it, 0) != pdTRUE) {
      RxItem dropped;  // queue full: drop the oldest, the totals are in the next reading anyway
      xQueueReceive(upQueue, &dropped, 0);
      xQueueSend(upQueue, &it, 0);
    }
    if (!upload && isNew) continue;  // "still here": nothing to print
    Serial.printf("Reader %s (%s): reading %lu, %d dBm%s%s\n", macStr(it.mac).c_str(), it.r.machine,
                  (unsigned long)it.r.seq, it.rssi, isNew ? "" : " (repeat)",
                  (it.r.run & LINK_RUN_IN_USE) ? ((it.r.run & LINK_RUN_ACTIVE) ? ", machine running" : ", machine stopped") : "");
  }
}

static void setNodeStatus(const uint8_t *mac, const String &msg) {
  portENTER_CRITICAL(&nodesMux);
  for (int i = 0; i < MAX_NODES; i++)
    if (nodes[i].used && memcmp(nodes[i].mac, mac, 6) == 0) strncpy(nodes[i].status, msg.c_str(), sizeof nodes[i].status - 1);
  portEXIT_CRITICAL(&nodesMux);
}

// Called by the sender task: upload what the readers sent
static void uploadNodeReadings() {
  if (!upQueue) return;
  RxItem it;
  while (WiFi.status() == WL_CONNECTED && xQueueReceive(upQueue, &it, 0) == pdTRUE) {
    JsonDocument doc;
    JsonObject p = doc["payload"].to<JsonObject>();
    p["machine"] = it.r.machine;
    p["kind"] = it.r.kind;
    p["device"] = macStr(it.mac);
    p["fw"] = it.r.fw;
    p["board"] = "xiao-esp32c6";
    p["boot"] = it.r.boot;
    p["seq"] = it.r.seq;
    p["uptime_s"] = it.r.uptime_s;
    p["rssi"] = it.rssi;
    p["wakes"] = it.r.wakes;
    if (it.r.battery_mv > 2000) p["battery_v"] = serialized(String(it.r.battery_mv / 1000.0f, 2));
    p["power"] = it.r.power ? "always_on" : "battery";
    p["link"] = "espnow";
    p["via"] = WiFi.macAddress();
    JsonArray arr = p["channels"].to<JsonArray>();
    for (int c = 0; c < it.r.nch; c++) {
      JsonObject o = arr.add<JsonObject>();
      char name[9] = {0};
      memcpy(name, it.r.ch[c].name, 8);
      o["name"] = name;
      o["enabled"] = it.r.ch[c].enabled != 0;
      o["pulses"] = (unsigned long long)it.r.ch[c].pulses;
      o["lpp"] = it.r.ch[c].lpp;
      o["litres"] = (double)it.r.ch[c].pulses * (double)it.r.ch[c].lpp;
    }
    // Machine running: state now and each start / stop, as "seconds ago" (the
    // reader's clock only counts from its power-up; Supabase turns it into a time)
    if ((it.r.run & LINK_RUN_IN_USE) || it.offN || it.backN) {
      JsonObject run = p["run"].to<JsonObject>();
      if (it.r.run & LINK_RUN_IN_USE) run["active"] = (it.r.run & LINK_RUN_ACTIVE) != 0;
      JsonArray evs = run["events"].to<JsonArray>();
      uint32_t waited = (millis() - it.rxMs) / 1000;  // time in the Atom's queue
      if (it.offN) {  // switched off since the last time we heard it
        JsonObject o = evs.add<JsonObject>();
        o["n"] = it.offN;
        o["on"] = false;
        o["power_off"] = true;
        o["ago_s"] = it.offAgoS + waited;
      }
      if (it.backN) {  // back after a silence that was reported as "off" (no restart)
        JsonObject o = evs.add<JsonObject>();
        o["n"] = it.backN;
        o["on"] = (it.r.run & LINK_RUN_ACTIVE) != 0;
        o["ago_s"] = waited;
      }
      for (int e = 0; e < it.r.nev; e++) {
        const LinkRunEvent &ev = it.r.ev[e];
        JsonObject o = evs.add<JsonObject>();
        o["n"] = ev.n;
        o["on"] = (ev.on & LINK_EV_ON) != 0;
        if (ev.on & LINK_EV_POWERUP) o["power_up"] = true;
        o["ago_s"] = (it.r.uptime_s >= ev.t_s ? it.r.uptime_s - ev.t_s : 0) + waited;
      }
    }
    String msg;
    postReading(doc, msg);
    setNodeStatus(it.mac, msg);
  }
}

// ----------------------------------------------------------------------------
// This week (Monday 00:00 to Sunday 24:00, local time)
// The litres used this week come from Supabase (view daily_usage, the same
// numbers as the web page), so they survive restarts and match the page.
// Between fetches the screen adds the pulses counted since the last reading
// the server has (view latest_totals).
// ----------------------------------------------------------------------------
struct WeekInfo {
  char mac[18];
  uint8_t nch;
  char ch[LINK_MAX_CH][9];
  double litres[LINK_MAX_CH];    // used this week, up to the server's last reading
  uint64_t pulses[LINK_MAX_CH];  // counter in the server's last reading
  bool hasPulses[LINK_MAX_CH];
};
static const int MAX_WEEK = 40;
static WeekInfo weekInfo[MAX_WEEK];
static int weekCount = 0;
static char weekStart[11] = "";   // Monday of the loaded week ("" = not loaded yet)
static portMUX_TYPE weekMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t weekFetchedMs = 0, weekTriedMs = 0;
static const uint32_t WEEK_REFRESH_MS = 30UL * 60 * 1000;  // also refresh every 30 min
static const uint32_t WEEK_RETRY_MS = 60UL * 1000;

// Monday of the current week as YYYY-MM-DD (local time). False until the clock is set.
static bool mondayKey(char out[11]) {
  time_t now = time(nullptr);
  if (now < 1600000000) return false;
  struct tm t;
  localtime_r(&now, &t);
  t.tm_mday -= (t.tm_wday + 6) % 7;  // tm_wday: 0 = Sunday
  t.tm_hour = 12;                    // midday: safe on clock-change days
  t.tm_min = t.tm_sec = 0;
  t.tm_isdst = -1;
  time_t mon = mktime(&t);
  localtime_r(&mon, &t);
  strftime(out, 11, "%Y-%m-%d", &t);
  return true;
}

static bool supabaseGet(const String &pathAndQuery, JsonDocument &doc) {
  if (cfg.url.isEmpty() || cfg.key.isEmpty()) return false;
  String url = cfg.url;
  while (url.endsWith("/")) url.remove(url.length() - 1);
  url += "/rest/v1/" + pathAndQuery;
  WiFiClientSecure client;
  client.setCACert(SUPABASE_ROOT_CAS);
  HTTPClient http;
  http.setTimeout(20000);
  if (!http.begin(client, url)) return false;
  http.addHeader("apikey", cfg.key);
  if (cfg.key.startsWith("eyJ")) http.addHeader("Authorization", "Bearer " + cfg.key);
  http.addHeader("Accept", "application/json");
  int code = http.GET();
  bool ok = false;
  if (code == 200) ok = !deserializeJson(doc, http.getString()) && doc.is<JsonArray>();
  else Serial.printf("Week fetch: HTTP %d for %s\n", code, pathAndQuery.c_str());
  http.end();
  return ok;
}

// Built by fetchWeek(), then copied into weekInfo
static WeekInfo weekTmp[MAX_WEEK];
static int weekTmpCount = 0;

// Entry for (mac, channel) in weekTmp, added when missing. Returns the channel slot or -1.
// (No WeekInfo in the parameters: the Arduino IDE declares functions before the struct.)
static int weekSlot(const char *mac, const char *ch) {
  WeekInfo *arr = weekTmp;
  int &count = weekTmpCount;
  int i = 0;
  for (; i < count; i++) if (strcmp(arr[i].mac, mac) == 0) break;
  if (i == count) {
    if (count >= MAX_WEEK) return -1;
    memset(&arr[i], 0, sizeof arr[i]);
    strncpy(arr[i].mac, mac, sizeof arr[i].mac - 1);
    count++;
  }
  WeekInfo &w = arr[i];
  for (int c = 0; c < w.nch; c++) if (strncmp(w.ch[c], ch, 8) == 0) return i * LINK_MAX_CH + c;
  if (w.nch >= LINK_MAX_CH) return -1;
  strncpy(w.ch[w.nch], ch, 8);
  return i * LINK_MAX_CH + w.nch++;
}

static bool fetchWeek() {
  char mon[11];
  if (!mondayKey(mon)) return false;
  WeekInfo *tmp = weekTmp;
  weekTmpCount = 0;

  // device id -> MAC
  JsonDocument devs;
  if (!supabaseGet("devices?select=id,mac", devs)) return false;
  auto macOf = [&](long id) -> const char * {
    for (JsonObject d : devs.as<JsonArray>()) if (d["id"].as<long>() == id) return d["mac"] | "";
    return "";
  };

  JsonDocument doc;
  if (!supabaseGet(String("daily_usage?select=device_id,channel,used_l&day=gte.") + mon, doc)) return false;
  for (JsonObject r : doc.as<JsonArray>()) {
    const char *mac = macOf(r["device_id"].as<long>());
    if (!*mac) continue;
    int s = weekSlot(mac, r["channel"] | "Water");
    if (s >= 0) tmp[s / LINK_MAX_CH].litres[s % LINK_MAX_CH] += r["used_l"].as<double>();
  }
  doc.clear();
  if (!supabaseGet("latest_totals?select=device_id,channel,pulses", doc)) return false;
  for (JsonObject r : doc.as<JsonArray>()) {
    const char *mac = macOf(r["device_id"].as<long>());
    if (!*mac || r["pulses"].isNull()) continue;
    int s = weekSlot(mac, r["channel"] | "Water");
    if (s < 0) continue;
    tmp[s / LINK_MAX_CH].pulses[s % LINK_MAX_CH] = r["pulses"].as<unsigned long long>();
    tmp[s / LINK_MAX_CH].hasPulses[s % LINK_MAX_CH] = true;
  }

  portENTER_CRITICAL(&weekMux);
  memcpy(weekInfo, tmp, sizeof(WeekInfo) * weekTmpCount);
  weekCount = weekTmpCount;
  memcpy(weekStart, mon, sizeof weekStart);
  portEXIT_CRITICAL(&weekMux);
  Serial.printf("Week from %s loaded: %d meters\n", mon, weekTmpCount);
  return true;
}

// Called by the sender task: refresh after uploads, every 30 min and when a new week starts.
static void maybeFetchWeek() {
  char mon[11];
  if (!mondayKey(mon)) return;
  bool newWeek = strcmp(mon, weekStart) != 0;
  bool due = newWeek || millis() - weekFetchedMs >= WEEK_REFRESH_MS ||
             (weekDirty && millis() - weekDirtyMs >= 5000);  // let a burst of uploads finish
  if (!due || (weekTriedMs && millis() - weekTriedMs < WEEK_RETRY_MS && !weekDirty)) return;
  weekTriedMs = millis();
  weekDirty = false;
  if (fetchWeek()) weekFetchedMs = millis();
}

// Litres used this week by one inlet. pulses/lpp: what the meter counts now.
// False when unknown (clock not set, not fetched yet, or never sent to Supabase).
static bool weekLitres(const String &mac, const char *ch, uint64_t pulses, float lpp, double &out) {
  char mon[11];
  if (!mondayKey(mon)) return false;
  bool ok = false;
  portENTER_CRITICAL(&weekMux);
  if (strcmp(mon, weekStart) == 0) {
    for (int i = 0; i < weekCount && !ok; i++) {
      if (strcmp(weekInfo[i].mac, mac.c_str()) != 0) continue;
      for (int c = 0; c < weekInfo[i].nch; c++) {
        if (strncmp(weekInfo[i].ch[c], ch, 8) != 0) continue;
        out = weekInfo[i].litres[c];
        if (weekInfo[i].hasPulses[c] && pulses > weekInfo[i].pulses[c])
          out += (double)(pulses - weekInfo[i].pulses[c]) * (double)lpp;
        ok = true;
        break;
      }
    }
  }
  portEXIT_CRITICAL(&weekMux);
  return ok;
}

// Called by the sender task: a mains powered reader that has been silent for
// 2.5 min was switched off with its machine. Report it as off from the last
// time we heard it (the reader's power-up later ends that period).
static const uint32_t SILENT_OFF_MS = 150UL * 1000;
static void reportSilentNodes() {
  for (int i = 0; i < MAX_NODES; i++) {
    NodeInfo n;
    portENTER_CRITICAL(&nodesMux);
    n = nodes[i];
    portEXIT_CRITICAL(&nodesMux);
    uint32_t now = millis();
    if (!n.used || !(n.run & LINK_KEEPALIVE) || n.offReported || now - n.lastSeenMs < SILENT_OFF_MS) continue;
    if (n.offTryMs && now - n.offTryMs < 60000UL) continue;
    uint16_t k = n.offK + 1;
    char machine[34];
    strncpy(machine, n.machine, sizeof machine);
    machine[33] = 0;
    JsonDocument doc;
    JsonObject p = doc["payload"].to<JsonObject>();
    p["report"] = "power_off";
    p["device"] = macStr(n.mac);
    p["machine"] = machine;
    p["boot"] = n.boot;
    p["via"] = WiFi.macAddress();
    JsonObject o = p["run"]["events"].to<JsonArray>().add<JsonObject>();
    o["n"] = -(int32_t)(2 * k - 1);
    o["on"] = false;
    o["power_off"] = true;
    o["ago_s"] = (now - n.lastSeenMs) / 1000;
    String msg;
    bool ok = postReading(doc, msg);
    Serial.printf("Reader %s (%s) silent for %lu s: machine off -> %s\n", macStr(n.mac).c_str(), machine,
                  (unsigned long)((now - n.lastSeenMs) / 1000), msg.c_str());
    portENTER_CRITICAL(&nodesMux);
    if (nodes[i].used && memcmp(nodes[i].mac, n.mac, 6) == 0 && nodes[i].lastSeenMs == n.lastSeenMs) {
      nodes[i].offTryMs = now ? now : 1;
      if (ok) {
        nodes[i].offReported = true;
        nodes[i].offK = k;
        strncpy(nodes[i].status, "machine off", sizeof nodes[i].status - 1);
      }
    }
    portEXIT_CRITICAL(&nodesMux);
  }
}

static void startGateway() {
  if (gatewayRunning || !cfg.gateway) return;
  WiFi.setSleep(false);  // keep the radio listening for the readers
  if (esp_now_init() != ESP_OK) { Serial.println("ESP-NOW init failed"); return; }
  rxQueue = xQueueCreate(16, sizeof(RxItem));
  upQueue = xQueueCreate(24, sizeof(RxItem));
  esp_now_register_recv_cb(onLinkRecv);
  xTaskCreatePinnedToCore(gatewayTask, "gateway", 6144, nullptr, 5, nullptr, 0);
  gatewayRunning = true;
  Serial.printf("Gateway on: listening for readers on channel %d\n", (int)WiFi.channel());
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
        if (cfg.ownMeter) doSend();
      }
      uploadNodeReadings();
      reportSilentNodes();
      maybeFetchWeek();
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
  if (cfg.ownMeter) {
    h += "<h2>Current readings</h2><table><tr><th>Inlet</th><th>Pulses</th><th>Litres</th></tr>";
    for (int i = 0; i < NUM_CH; i++) {
      h += "<tr><td>" + String(CH_NAMES[i]) + (st[i].en ? "" : " (off)") + "</td><td>" +
           String((unsigned long long)getPulses(i)) + "</td><td>" + String(litres(i), 1) + "</td></tr>";
    }
    h += "</table>";
  }
  h += "<div class='muted'>Wi-Fi: " +
       (WiFi.status() == WL_CONNECTED ? esc(WiFi.SSID()) + " (" + WiFi.localIP().toString() + ", " + String(WiFi.RSSI()) + " dBm)" : String("not connected")) +
       " &middot; last send: " + esc(lastSendMsg) + "</div>";

  // Readers heard over ESP-NOW
  if (cfg.gateway) {
    h += "<h2>XIAO readers</h2>";
    if (!gatewayRunning) h += "<div class='muted'>Starts when the Atom is on the Wi-Fi.</div>";
    else h += "<div class='muted'>Listening on channel " + String(WiFi.channel()) + " &middot; " + String(gwReceived) + " readings received" +
              (gwBadKey ? " &middot; " + String(gwBadKey) + " with a different network key" : String("")) + "</div>";
    h += "<table><tr><th>Reader</th><th>Last heard</th><th>Signal</th><th>Battery</th><th>Pulses</th><th>Machine</th><th>Supabase</th></tr>";
    int shown = 0;
    for (int i = 0; i < MAX_NODES; i++) {
      NodeInfo n;
      portENTER_CRITICAL(&nodesMux);
      n = nodes[i];
      portEXIT_CRITICAL(&nodesMux);
      if (!n.used) continue;
      shown++;
      uint32_t ago = (millis() - n.lastSeenMs) / 1000;
      String pulses;
      for (int c = 0; c < n.nch; c++) {
        if (!n.enabled[c]) continue;
        if (pulses.length()) pulses += " / ";
        pulses += String((unsigned long long)n.pulses[c]);
      }
      char machine[34];
      strncpy(machine, n.machine, sizeof machine);
      machine[33] = 0;
      h += "<tr><td>" + esc(machine) + "<br><span class='muted'>" + macStr(n.mac) + "</span></td><td>" +
           (ago < 120 ? String(ago) + " s ago" : String(ago / 60) + " min ago") + "</td><td>" + String(n.rssi) + " dBm</td><td>" +
           (n.battMv > 2000 ? String(n.battMv / 1000.0f, 2) + " V" : String("-")) + "</td><td>" + pulses + "</td><td>" +
           (n.offReported ? String("<b>switched off</b>") :
            (n.run & LINK_RUN_IN_USE) ? String((n.run & LINK_RUN_ACTIVE) ? "<b>running</b>" : "stopped") : String("-")) +
           "</td><td>" + esc(String(n.status)) + "</td></tr>";
    }
    if (!shown) h += "<tr><td colspan='7' class='muted'>None yet. Press BOOT on a reader to make it send.</td></tr>";
    h += "</table>";
  }

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
  h += "<label><input type='checkbox' name='own' value='1'" + String(cfg.ownMeter ? " checked" : "") + "> This Atom also reads a machine (its own inlets, settings below)</label>";
  h += "<label><input type='checkbox' name='gw' value='1'" + String(cfg.gateway ? " checked" : "") + "> Gateway: receive the battery readers (XIAO) and send their readings</label>";
  h += "<label>Network key (the same on every XIAO reader)</label><input name='mkey' maxlength='32' value='" + esc(cfg.meshKey) + "'>";
  h += "<label>Time zone (POSIX)</label><input name='tz' value='" + esc(cfg.tz) + "'>";
  h += "<div class='muted'>Ireland: GMT0IST,M3.5.0/1,M10.5.0</div>";
  h += "<label>Password for this page, user admin (blank = keep)</label><input name='adm' type='password'>";
  h += "</fieldset>";

  if (cfg.ownMeter) h += "<h2>Water inlets</h2>";
  for (int i = 0; i < NUM_CH && cfg.ownMeter; i++) {
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
  if (cfg.ownMeter) {
  h += "<h2>Match the count to the 7111</h2><form method='post' action='/counts'><fieldset><legend>Current pulses</legend>";
  h += "<div class='muted'>Type the number shown on each 7111 counter. Blank = leave as is.</div>";
  for (int i = 0; i < NUM_CH; i++) {
    h += "<label>" + String(CH_NAMES[i]) + " (now: " + String((unsigned long long)getPulses(i)) + ")</label><input name='p" + String(i) + "' type='number' min='0' step='1'>";
  }
  h += "</fieldset><button type='submit'>Save count</button></form>";

  h += "<h2>Test</h2><form method='post' action='/send'><button type='submit'>Send a reading now</button></form>";
  }
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
  n.gateway = server.hasArg("gw");
  n.ownMeter = server.hasArg("own");
  if (!n.ownMeter) n.gateway = true;
  n.meshKey = server.arg("mkey"); n.meshKey.trim();
  if (n.meshKey.isEmpty()) n.meshKey = MESH_KEY;
  n.tz = server.arg("tz"); n.tz.trim();
  if (n.tz.isEmpty()) n.tz = "GMT0";
  if (server.arg("adm").length()) n.adminPass = server.arg("adm");

  String err;
  // The inlet fields are only on the page when this Atom reads a machine
  for (int i = 0; i < NUM_CH && server.hasArg("c0lpp"); i++) {
    String p = "c" + String(i);
    n.ch[i].enabled = server.hasArg(p + "en");
    n.ch[i].pin = (uint8_t)server.arg(p + "pin").toInt();
    n.ch[i].lpp = server.arg(p + "lpp").toFloat();
    n.ch[i].mode = server.arg(p + "mod").toInt() == 1 ? MODE_PULLUP : MODE_PARALLEL_7111;
    if (!pinAllowed(n.ch[i].pin)) err += "Invalid pin for " + String(CH_NAMES[i]) + ". ";
    if (n.ch[i].lpp <= 0) err += "Invalid litres per pulse for " + String(CH_NAMES[i]) + ". ";
  }
  for (int i = 0; i < NUM_CH && n.ownMeter; i++)
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
static uint16_t chColor[NUM_CH];
static const uint16_t COUNTER_COLOR = 0xFE60;  // amber, like the 7111 digits

// Always whole litres, so the three inlets use the same unit.
static String fmtLitres(double l) {
  if (l < 0) l = 0;
  return String((unsigned long)(l + 0.5)) + " L";
}

// One inlet row, 28 px high:
//   [C]  week          1234 L     <- litres used this week (Mon-Sun)
//        tot 45678 L   0045678    <- total litres | counter (compare with the 7111)
static void drawInletRow(int y, int c, bool en, bool weekOk, double weekL, double totalL, uint64_t pulses, bool flash) {
  canvas.fillRoundRect(0, y, 18, 28, 3, en ? chColor[c] : 0x4208);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(TFT_BLACK);
  canvas.setTextDatum(MC_DATUM);
  canvas.drawString(CH_SHORT[c], 9, y + 14);
  if (flash) canvas.fillCircle(13, y + 4, 2, TFT_WHITE);  // a pulse just came in
  if (!en) {
    canvas.setTextDatum(MR_DATUM);
    canvas.setTextColor(0x8410);
    canvas.drawString("off", 126, y + 14);
    canvas.setTextDatum(TL_DATUM);
    return;
  }
  canvas.setFont(&fonts::Font0);
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextColor(0x8410);
  canvas.drawString("week", 23, y + 4);
  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(TR_DATUM);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString(weekOk ? fmtLitres(weekL) : String("--"), 126, y);

  canvas.setFont(&fonts::Font0);
  String counter = String((unsigned long long)pulses);
  String total = "tot " + fmtLitres(totalL);
  if (canvas.textWidth(total) + canvas.textWidth(counter) > 98) total = fmtLitres(totalL);
  canvas.setTextColor(0xAD55);
  canvas.setTextDatum(TL_DATUM);
  canvas.drawString(total, 23, y + 19);
  canvas.setTextColor(COUNTER_COLOR);
  canvas.setTextDatum(TR_DATUM);
  canvas.drawString(counter, 126, y + 19);
  canvas.setTextDatum(TL_DATUM);
}

// Gateway-only screen. Page 0: one row per reader with this week's litres.
// Tap the screen: next page = one machine with week / total / counter per inlet.
static int gwPage = 0;

static void drawGateway() {
  NodeInfo list[MAX_NODES];
  int n = 0;
  portENTER_CRITICAL(&nodesMux);
  for (int i = 0; i < MAX_NODES; i++) if (nodes[i].used) list[n++] = nodes[i];
  portEXIT_CRITICAL(&nodesMux);
  // Pages in a stable order: by machine name
  for (int a = 0; a < n; a++)
    for (int b = a + 1; b < n; b++)
      if (strncmp(list[b].machine, list[a].machine, sizeof list[a].machine) < 0) { NodeInfo t = list[a]; list[a] = list[b]; list[b] = t; }
  if (gwPage > n) gwPage = 0;

  canvas.fillScreen(TFT_BLACK);
  canvas.setTextDatum(TL_DATUM);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(TFT_WHITE);

  if (gwPage == 0) {
    // ---- list of machines
    canvas.drawString("This week", 2, 0);
    canvas.setTextDatum(TR_DATUM);
    canvas.setTextColor(0x8410);
    canvas.drawString(String(n), 126, 0);
    canvas.setTextDatum(TL_DATUM);
    canvas.drawFastHLine(0, 17, 128, 0x4208);
    canvas.setFont(&fonts::Font0);
    if (n == 0) {
      canvas.setTextColor(0x8410);
      canvas.drawString("No readers yet.", 2, 30);
      canvas.drawString("Press BOOT on a", 2, 46);
      canvas.drawString("XIAO to send.", 2, 58);
    }
    for (int i = 0; i < n && i < 6; i++) {
      int y = 21 + i * 15;
      uint32_t ago = (millis() - list[i].lastSeenMs) / 60000;
      bool late = ago > 2UL * cfg.intervalMin + 5;
      char name[34];
      strncpy(name, list[i].machine, sizeof name);
      name[33] = 0;
      double week = 0;
      bool weekOk = true;
      String mac = macStr(list[i].mac);
      for (int c = 0; c < list[i].nch; c++) {
        if (!list[i].enabled[c]) continue;
        double w;
        if (weekLitres(mac, list[i].chName[c], list[i].pulses[c], list[i].lpp[c], w)) week += w;
        else weekOk = false;
      }
      String right = weekOk ? fmtLitres(week) : String("--");
      String nm = name;
      canvas.setTextColor(late ? TFT_ORANGE : TFT_WHITE);
      while (nm.length() > 1 && canvas.textWidth(nm) + canvas.textWidth(right) > 112) nm.remove(nm.length() - 1);
      // Green dot = machine running now
      if ((list[i].run & LINK_RUN_ACTIVE) && !late) canvas.fillCircle(3, y + 3, 2, TFT_GREEN);
      canvas.drawString(nm, 8, y);
      canvas.setTextDatum(TR_DATUM);
      canvas.setTextColor(late ? TFT_ORANGE : 0xAD55);
      canvas.drawString(right, 126, y);
      canvas.setTextDatum(TL_DATUM);
    }
    if (n > 6) { canvas.setTextColor(0x8410); canvas.drawString("+" + String(n - 6) + " more, tap", 2, 21 + 6 * 15); }
  } else {
    // ---- one machine
    const NodeInfo &m = list[gwPage - 1];
    char name[34];
    strncpy(name, m.machine, sizeof name);
    name[33] = 0;
    String title = name;
    while (title.length() > 1 && canvas.textWidth(title) > 100) title.remove(title.length() - 1);
    canvas.drawString(title, 2, 0);
    canvas.setTextDatum(TR_DATUM);
    canvas.setTextColor(0x8410);
    canvas.drawString(String(gwPage) + "/" + String(n), 126, 0);
    canvas.drawFastHLine(0, 17, 128, 0x4208);
    String mac = macStr(m.mac);
    for (int c = 0; c < NUM_CH; c++) {
      bool en = c < m.nch && m.enabled[c];
      double w = 0;
      bool wOk = en && weekLitres(mac, m.chName[c], m.pulses[c], m.lpp[c], w);
      drawInletRow(21 + c * 30, c, en, wOk, w, en ? (double)m.pulses[c] * m.lpp[c] : 0, en ? m.pulses[c] : 0, false);
    }
    canvas.setTextDatum(TL_DATUM);
    canvas.drawFastHLine(0, 112, 128, 0x4208);
    canvas.setFont(&fonts::Font0);
    uint32_t ago = (millis() - m.lastSeenMs) / 60000;
    String foot = ago < 60 ? String(ago) + " min ago" : String(ago / 60) + " h ago";
    canvas.setTextColor(ago > 2UL * cfg.intervalMin + 5 ? TFT_ORANGE : TFT_GREENYELLOW);
    canvas.drawString(foot, 2, 117);
    if (m.offReported || (m.run & LINK_RUN_IN_USE)) {
      canvas.setTextDatum(TC_DATUM);
      bool on = !m.offReported && (m.run & LINK_RUN_ACTIVE);
      canvas.setTextColor(m.offReported ? TFT_ORANGE : on ? TFT_GREEN : 0x8410);
      canvas.drawString(m.offReported ? "OFF" : on ? "RUN" : "STOP", 72, 117);
      canvas.setTextDatum(TL_DATUM);
    }
    canvas.setTextColor(0x8410);
    canvas.setTextDatum(TR_DATUM);
    canvas.drawString(m.battMv > 2000 ? String(m.battMv / 1000.0f, 2) + " V" : String(m.rssi) + " dBm", 126, 117);
    canvas.setTextDatum(TL_DATUM);
    canvas.pushSprite(0, 0);
    return;
  }

  // footer for the list page
  canvas.setFont(&fonts::Font0);
  canvas.drawFastHLine(0, 112, 128, 0x4208);
  if (WiFi.status() != WL_CONNECTED) { canvas.setTextColor(TFT_ORANGE); canvas.drawString("no Wi-Fi", 2, 117); }
  else { canvas.setTextColor(TFT_GREENYELLOW); canvas.drawString("gateway ch " + String((int)WiFi.channel()), 2, 117); }
  canvas.setTextColor(0x8410);
  canvas.setTextDatum(TR_DATUM);
  canvas.drawString(clockStr(time(nullptr)), 126, 117);
  canvas.setTextDatum(TL_DATUM);
  canvas.pushSprite(0, 0);
}

static void drawScreen() {
  if (!cfg.ownMeter && !configMode) { drawGateway(); return; }
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
    canvas.drawString(cfg.ownMeter ? "Still counting" : "Gateway paused", 2, 116);
    canvas.pushSprite(0, 0);
    return;
  }

  // 3 rows: Cold / Warm / Hot, each with this week / total / counter
  String myMac = WiFi.macAddress();
  for (int i = 0; i < NUM_CH; i++) {
    uint64_t p = getPulses(i);
    double w = 0;
    bool wOk = st[i].en && weekLitres(myMac, CH_NAMES[i], p, cfg.ch[i].lpp, w);
    bool flash = millis() - st[i].lastPulseMs < 300 && st[i].lastPulseMs != 0;
    drawInletRow(21 + i * 30, i, st[i].en, wOk, w, (double)p * cfg.ch[i].lpp, p, flash);
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
  canvas.drawString(clockStr(time(nullptr)), 126, 117);
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

  // Button: tap = next machine (gateway screen); 5 s = SETUP mode
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
    if (!longHandled && !wakePress) {
      if (!cfg.ownMeter) gwPage++;  // next machine (wraps back to the list)
    }
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
  if (conn && !wasConnected && !configMode) {
    ipShownAt = millis();
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    startGateway();
  }
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
