// =============================================================================
//  Water meter reader - Seeed Studio XIAO ESP32-C6 (battery + deep sleep)
//
//  Counts up to 3 water meters (Cold, Warm, Hot) from the same reed contact
//  that drives each Trumeter 7111 counter, and sends the totals over ESP-NOW
//  to the AtomS3R gateway, which forwards them to Supabase.
//
//  "Machine running" input (optional): a contact or optocoupler to GND that
//  tells when the machine is working. Each change (start / stop) is sent to
//  the gateway at once, with the time it happened, so the web page can show
//  when the machine ran and when it stood still. A change the gateway did not
//  confirm is kept (up to 16) and goes with the next send.
//
//  Power: "Always on" (default) for units fed from the machine's 5 V. Every
//  pin can be used, the readings go out every "Send every (min)" and a short
//  "still here" goes to the gateway every minute: when the machine is switched
//  off (and the XIAO with it) the gateway notices the silence and reports the
//  machine as off; the XIAO's power-up ends that period.
//
//  Firmware update over Wi-Fi: on the Atom's page, "Open setup" on a reader
//  makes it open its SETUP Wi-Fi at its next send (within a minute). Connect
//  to "WaterMeter-XXXX" and open http://192.168.4.1/update (user admin and
//  this page's password), then choose the .bin from Arduino's
//  Sketch > Export Compiled Binary. The counts are kept.
//
//  How it saves battery:
//  - Between pulses the chip is in deep sleep (~15 uA). Each change of a
//    contact (close or open) wakes it for a fraction of a second to count.
//  - The radio is only switched on to send, every "Send every (min)", and
//    only for a fraction of a second: no router, no internet.
//
//  After power-up or the RESET button (the "test window"):
//  - The yellow LED blinks fast for 3 s. Press BOOT during those 3 s to enter
//    SETUP (the XIAO's own Wi-Fi "WaterMeter-XXXX", password "watermeter",
//    page http://192.168.4.1/). Before the first set-up it goes there by itself.
//  - Then it stays awake for 10 minutes: the LED flashes on each pulse, the
//    serial monitor shows each pulse, and a short press on BOOT sends a
//    reading (3 short LED blinks = the gateway got it, 1 long = no answer).
//  - After the 10 minutes it goes to deep sleep (Power mode "Battery"), or
//    stays awake for good (Power mode "Always on", for USB-powered units).
//
//  Pins (contacts to GND, internal pull-up, through a 1N4148 diode to the 7111):
//    Cold = D0 (GPIO0), Warm = D1 (GPIO1), Hot = D2 (GPIO2)
//    Machine running = D3 (GPIO21): dry relay contact between D3 and GND (no 5 V!)
//    Battery voltage: 2 x 1 MOhm divider from BAT+ to the MTMS pad (GPIO4).
//  Only GPIO0-7 can wake the ESP32-C6 from deep sleep, so in "Battery" mode
//  every input must be on D0, D1, D2 or the MTDI/MTCK/MTDO pads underneath.
//  In "Always on" mode D3 to D10 can be used as well.
// =============================================================================
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_timer.h>
#include <esp_sleep.h>
#include <esp_mac.h>
#include <driver/gpio.h>
#include <soc/soc_caps.h>
#include <esp_idf_version.h>
#include <time.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Update.h>

#define FW_VERSION "3.3.0-c6"

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#include "espnow_link.h"   // after secrets.h, which may set MESH_KEY
// ----------------------------------------------------------------------------
// Board pins (XIAO ESP32-C6)
// ----------------------------------------------------------------------------
static const int PIN_LED = 15;        // yellow user LED, on when LOW
static const int PIN_BOOT = 9;        // BOOT button, LOW when pressed
static const int PIN_RF_POWER = 3;    // RF switch power: LOW = on
static const int PIN_RF_SELECT = 14;  // LOW = built-in antenna, HIGH = U.FL
#ifndef BATT_PIN
#define BATT_PIN 4                    // MTMS pad (ADC). Set to -1 if there is no divider.
#endif
static const float BATT_DIVIDER = 2.0f;  // two equal resistors

// ----------------------------------------------------------------------------
// Constants
// ----------------------------------------------------------------------------
static const int NUM_CH = 3;
static const char *CH_NAMES[NUM_CH] = {"Cold", "Warm", "Hot"};
// Deep-sleep wake capable pins that are reachable on the XIAO (GPIO4 is the battery)
static const uint8_t ALLOWED_PINS[] = {0, 1, 2, 5, 6, 7};
static const char *PIN_LABELS[] = {"D0", "D1", "D2", "MTDI pad (G5)", "MTCK pad (G6)", "MTDO pad (G7)"};
static const uint8_t DEFAULT_PINS[NUM_CH] = {0, 1, 2};
// Pins for the "machine running" input: the ones above, then D3-D10 (Always on mode only)
static const uint8_t RUN_PINS[] = {0, 1, 2, 5, 6, 7, 21, 22, 23, 16, 17, 19, 20, 18};
static const char *RUN_PIN_LABELS[] = {"D0", "D1", "D2", "MTDI pad (G5)", "MTCK pad (G6)", "MTDO pad (G7)",
                                       "D3", "D4", "D5", "D6", "D7", "D8", "D9", "D10"};
static const uint8_t DEFAULT_RUN_PIN = 21;  // D3
static const uint16_t RUN_FILTER_MIN_MS = 20, RUN_FILTER_MAX_MS = 10000;
static const uint32_t RUN_SEND_GAP_MS = 3000;  // at most one immediate send per 3 s while awake
static const uint32_t ALIVE_EVERY_MS = 60000;  // Always on: "still here" to the gateway (detects the machine switched off)

static const uint32_t TEST_WINDOW_MS = 10UL * 60 * 1000;   // awake after power-up / reset
static const uint32_t SETUP_PRESS_WINDOW_MS = 3000;        // press BOOT within this after reset
static const uint32_t SAVE_EVERY_MS = 30UL * 1000;         // save counts to flash while awake
static const uint32_t SAVE_EVERY_PULSES = 50;              // save counts to flash while sleeping
static const uint32_t FIRST_SEND_DELAY_MS = 20UL * 1000;
static const uint32_t RETRY_MIN = 5;                       // retry after a failed send (first 3 times)
static const uint32_t CONFIG_LONGPRESS_MS = 5000;
static const uint32_t CONFIG_AP_TIMEOUT_MS = 15UL * 60 * 1000;
static const char *AP_PASSWORD = "watermeter";

enum InputMode : uint8_t {
  MODE_PARALLEL_7111 = 0, // no pull-up (advanced: an external pull-up holds the level)
  MODE_PULLUP = 1         // ESP internal pull-up: via 1N4148 diode to the 7111, optocoupler or bare sensor
};
enum PowerMode : uint8_t { POWER_BATTERY = 0, POWER_ALWAYS_ON = 1 };

// ----------------------------------------------------------------------------
// Settings (flash, namespace "cfg")
// ----------------------------------------------------------------------------
struct ChannelCfg {
  bool enabled;
  uint8_t pin;
  float lpp;     // litres per pulse
  uint8_t mode;  // InputMode
};

struct Config {
  String machine;
  String kind;     // washer | boiler | canteen | other
  String adminPass;
  uint16_t intervalMin;
  uint16_t debounceMs;
  uint8_t power;   // PowerMode
  String meshKey;  // ESP-NOW network key, same as on the gateway
  bool configured; // settings saved at least once
  ChannelCfg ch[NUM_CH];
  bool runEnabled;     // "machine running" input in use
  uint8_t runPin;
  bool runActiveLow;   // true: running when the pin is pulled to GND (optocoupler / contact closed)
  uint16_t runFilterMs;// the input must hold a new level this long to count
} cfg;

Preferences prefs;

static void loadConfig() {
  prefs.begin("cfg", true);
  cfg.machine = prefs.getString("mach", "Machine 01");
  cfg.kind = prefs.getString("kind", "washer");
  cfg.adminPass = prefs.getString("adm", "watermeter");
  cfg.intervalMin = prefs.getUShort("int", 15);
  cfg.debounceMs = prefs.getUShort("deb", 30);
  cfg.power = prefs.getUChar("pwr", POWER_ALWAYS_ON);
  cfg.meshKey = prefs.getString("mkey", "");
  if (cfg.meshKey.isEmpty()) cfg.meshKey = MESH_KEY;
  cfg.configured = prefs.getBool("done", prefs.isKey("mach"));
  for (int i = 0; i < NUM_CH; i++) {
    char k[8];
    snprintf(k, sizeof k, "c%den", i);  cfg.ch[i].enabled = prefs.getBool(k, true);
    snprintf(k, sizeof k, "c%dpin", i); cfg.ch[i].pin = prefs.getUChar(k, DEFAULT_PINS[i]);
    snprintf(k, sizeof k, "c%dlpp", i); cfg.ch[i].lpp = prefs.getFloat(k, 1.0f);
    snprintf(k, sizeof k, "c%dmod", i); cfg.ch[i].mode = prefs.getUChar(k, MODE_PULLUP);
  }
  cfg.runEnabled = prefs.getBool("ren", false);
  cfg.runPin = prefs.getUChar("rpin", DEFAULT_RUN_PIN);
  cfg.runActiveLow = prefs.getBool("rlow", true);
  cfg.runFilterMs = prefs.getUShort("rflt", 1000);
  prefs.end();
  if (cfg.runFilterMs < RUN_FILTER_MIN_MS) cfg.runFilterMs = RUN_FILTER_MIN_MS;
  if (cfg.runFilterMs > RUN_FILTER_MAX_MS) cfg.runFilterMs = RUN_FILTER_MAX_MS;
  if (cfg.intervalMin < 1) cfg.intervalMin = 1;
  if (cfg.debounceMs < 2) cfg.debounceMs = 2;
  if (cfg.debounceMs > 500) cfg.debounceMs = 500;
}

static void saveConfig() {
  prefs.begin("cfg", false);
  prefs.putString("mach", cfg.machine);
  prefs.putString("kind", cfg.kind);
  prefs.putString("adm", cfg.adminPass);
  prefs.putUShort("int", cfg.intervalMin);
  prefs.putUShort("deb", cfg.debounceMs);
  prefs.putUChar("pwr", cfg.power);
  prefs.putString("mkey", cfg.meshKey);
  prefs.putBool("done", true);
  cfg.configured = true;
  for (int i = 0; i < NUM_CH; i++) {
    char k[8];
    snprintf(k, sizeof k, "c%den", i);  prefs.putBool(k, cfg.ch[i].enabled);
    snprintf(k, sizeof k, "c%dpin", i); prefs.putUChar(k, cfg.ch[i].pin);
    snprintf(k, sizeof k, "c%dlpp", i); prefs.putFloat(k, cfg.ch[i].lpp);
    snprintf(k, sizeof k, "c%dmod", i); prefs.putUChar(k, cfg.ch[i].mode);
  }
  prefs.putBool("ren", cfg.runEnabled);
  prefs.putUChar("rpin", cfg.runPin);
  prefs.putBool("rlow", cfg.runActiveLow);
  prefs.putUShort("rflt", cfg.runFilterMs);
  prefs.end();
}

static uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }

static int pinIndex(int p) {
  for (size_t i = 0; i < sizeof ALLOWED_PINS; i++) if (ALLOWED_PINS[i] == p) return (int)i;
  return -1;
}
static bool pinAllowed(int p) { return pinIndex(p) >= 0; }
static int runPinIndex(int p) {
  for (size_t i = 0; i < sizeof RUN_PINS; i++) if (RUN_PINS[i] == p) return (int)i;
  return -1;
}
// The running input can wake the chip only from GPIO0-7
static bool runPinOk(int p, uint8_t power) { return power == POWER_BATTERY ? pinAllowed(p) : runPinIndex(p) >= 0; }

// ----------------------------------------------------------------------------
// State kept in RTC memory during deep sleep (lost on power-up / reset, when
// the counts are read back from flash)
// ----------------------------------------------------------------------------
static const uint32_t RTC_MAGIC = 0x5741C601;
RTC_DATA_ATTR static uint32_t rtcMagic = 0;
RTC_DATA_ATTR static uint64_t rtcPulses[NUM_CH];
RTC_DATA_ATTR static uint64_t rtcSavedPulses[NUM_CH];  // last values written to flash
RTC_DATA_ATTR static uint8_t rtcLevel[NUM_CH];         // contact level when going to sleep (1 = open)
RTC_DATA_ATTR static time_t rtcNextSend = 0;           // seconds since power-up (time() keeps running in deep sleep)
RTC_DATA_ATTR static uint32_t rtcBootId = 0;
RTC_DATA_ATTR static uint32_t rtcSeq = 0;
RTC_DATA_ATTR static uint8_t rtcFails = 0;
RTC_DATA_ATTR static uint32_t rtcWakes = 0;
RTC_DATA_ATTR static uint8_t rtcLinkChannel = 0;        // gateway's channel, last time it answered
// "Machine running" input
static const int RUN_EV_MAX = 16;
RTC_DATA_ATTR static uint8_t rtcRunOn = 0;              // accepted state: 1 = running
RTC_DATA_ATTR static uint32_t rtcRunN = 0;              // changes since power-up
RTC_DATA_ATTR static LinkRunEvent rtcEv[RUN_EV_MAX];    // changes the gateway has not confirmed yet
RTC_DATA_ATTR static uint8_t rtcEvCount = 0;
RTC_DATA_ATTR static uint8_t rtcPoweredOn = 0;          // this boot started from no power (not a restart)
RTC_DATA_ATTR static uint8_t rtcOpenSetup = 0;          // the gateway asked for SETUP: open it after the restart
static volatile bool g_remoteSetup = false;             // the gateway asked for SETUP (answer to the last send)

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
static esp_timer_handle_t tickTimer = nullptr;
static volatile uint32_t g_lastAnyPulseMs = 0;

// "Machine running" input, sampled with the pulses
struct RunState {
  bool en;
  gpio_num_t pin;
  bool activeLow;
  uint8_t on;          // accepted state (1 = running)
  uint32_t run;        // consecutive ms at the other state
  uint32_t changeT;    // clock (s) when the other state was first seen
};
static RunState rs = {};
static volatile uint16_t g_runFilter = 1000;
static volatile bool g_runChanged = false;   // a change is waiting to be sent
static portMUX_TYPE runMux = portMUX_INITIALIZER_UNLOCKED;

static uint8_t runRaw() {
  uint8_t lvl = gpio_get_level(rs.pin) ? 1 : 0;
  return rs.activeLow ? !lvl : lvl;
}

// Keep a change until the gateway confirms it (the oldest is dropped when full)
static void pushRunEvent(uint8_t on, uint32_t t) {
  portENTER_CRITICAL(&runMux);
  if (rtcEvCount >= RUN_EV_MAX) {
    memmove(&rtcEv[0], &rtcEv[1], sizeof(LinkRunEvent) * (RUN_EV_MAX - 1));
    rtcEvCount--;
  }
  LinkRunEvent &e = rtcEv[rtcEvCount++];
  e.n = ++rtcRunN;
  e.t_s = t;
  e.on = on;
  rtcRunOn = on & LINK_EV_ON;
  g_runChanged = true;
  portEXIT_CRITICAL(&runMux);
}

// Forget the changes up to number n (the gateway has them)
static void dropRunEvents(uint32_t n) {
  portENTER_CRITICAL(&runMux);
  int k = 0;
  while (k < rtcEvCount && rtcEv[k].n <= n) k++;
  if (k) {
    memmove(&rtcEv[0], &rtcEv[k], sizeof(LinkRunEvent) * (rtcEvCount - k));
    rtcEvCount -= k;
  }
  if (rtcEvCount) g_runChanged = true;  // more than one packet's worth: send the rest soon
  portEXIT_CRITICAL(&runMux);
}

static void tickRun() {
  if (!rs.en) return;
  uint8_t v = runRaw();
  if (v != rs.on) {  // rs.on is 0 or 1
    if (rs.run == 0) rs.changeT = (uint32_t)time(nullptr);
    if (++rs.run >= g_runFilter) {
      rs.on = v;
      rs.run = 0;
      pushRunEvent(v, rs.changeT);
    }
  } else {
    rs.run = 0;
  }
}

static void tickOnce() {
  tickRun();
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
          g_lastAnyPulseMs = now;
        }
      }
    } else {
      c.run = 0;
    }
  }
}
static void tickCb(void *) { tickOnce(); }

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

static void loadCountsFromFlash() {
  prefs.begin("cnt", true);
  uint64_t v[NUM_CH] = {0, 0, 0};
  if (prefs.getBytesLength("p") == sizeof v) prefs.getBytes("p", v, sizeof v);
  prefs.end();
  for (int i = 0; i < NUM_CH; i++) { rtcPulses[i] = v[i]; rtcSavedPulses[i] = v[i]; }
}

static void saveCounts(bool force) {
  uint64_t v[NUM_CH];
  bool changed = false;
  for (int i = 0; i < NUM_CH; i++) {
    v[i] = getPulses(i);
    rtcPulses[i] = v[i];
    if (v[i] != rtcSavedPulses[i]) changed = true;
  }
  if (!changed && !force) return;
  prefs.begin("cnt", false);
  prefs.putBytes("p", v, sizeof v);
  prefs.end();
  for (int i = 0; i < NUM_CH; i++) rtcSavedPulses[i] = v[i];
}

static uint64_t pulsesSinceSave() {
  uint64_t n = 0;
  for (int i = 0; i < NUM_CH; i++) n += getPulses(i) - rtcSavedPulses[i];
  return n;
}

// Configure the inputs and load the counts from RTC memory
static void setupInputs() {
  g_debounce = cfg.debounceMs;
  for (int i = 0; i < NUM_CH; i++) {
    ChState &c = st[i];
    c.en = cfg.ch[i].enabled && pinAllowed(cfg.ch[i].pin);
    c.pin = (gpio_num_t)cfg.ch[i].pin;
    c.run = 0;
    c.lastPulseMs = 0;
    c.pulses = rtcPulses[i];
    if (!c.en) continue;
    gpio_hold_dis(c.pin);
    pinMode(c.pin, cfg.ch[i].mode == MODE_PULLUP ? INPUT_PULLUP : INPUT);
  }
  rs.en = cfg.runEnabled && runPinOk(cfg.runPin, cfg.power);
  rs.pin = (gpio_num_t)cfg.runPin;
  rs.activeLow = cfg.runActiveLow;
  rs.on = rtcRunOn;
  rs.run = 0;
  g_runFilter = cfg.runFilterMs;
  if (rs.en) {
    gpio_hold_dis(rs.pin);
    // Closed-to-GND contact: pull-up. Signal that brings 3.3 V: pull-down, so an open wire reads "stopped".
    pinMode(rs.pin, rs.activeLow ? INPUT_PULLUP : INPUT_PULLDOWN);
  }
}

static void startTickTimer() {
  esp_timer_create_args_t args = {};
  args.callback = &tickCb;
  args.name = "pulses";
  esp_timer_create(&args, &tickTimer);
  esp_timer_start_periodic(tickTimer, 1000);  // 1 ms
}

static void stopTickTimer() {
  if (!tickTimer) return;
  esp_timer_stop(tickTimer);
  esp_timer_delete(tickTimer);
  tickTimer = nullptr;
}

// ----------------------------------------------------------------------------
// Battery
// ----------------------------------------------------------------------------
static float batteryVolts() {
#if BATT_PIN >= 0
  uint32_t mv = 0;
  for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(BATT_PIN);
  float v = (mv / 16.0f) * BATT_DIVIDER / 1000.0f;
  return v > 2.0f ? v : 0.0f;  // 0 = no divider fitted
#else
  return 0.0f;
#endif
}

// ----------------------------------------------------------------------------
// LED
// ----------------------------------------------------------------------------
static void led(bool on) { digitalWrite(PIN_LED, on ? LOW : HIGH); }

// ----------------------------------------------------------------------------
// Sending
// ----------------------------------------------------------------------------
static String lastSendMsg = "waiting";
static bool lastSendOk = false;
static uint32_t lastSendOkMs = 0;
static float lastBattery = 0;

static String macString() {
  uint8_t m[6];
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  char b[18];
  snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return b;
}

static void scheduleNext(bool ok) {
  time_t now = time(nullptr);
  if (ok) {
    rtcFails = 0;
    rtcNextSend = now + (time_t)cfg.intervalMin * 60;
  } else {
    if (rtcFails < 255) rtcFails++;
    uint32_t m = rtcFails <= 3 ? umin(RETRY_MIN, cfg.intervalMin) : cfg.intervalMin;
    rtcNextSend = now + (time_t)m * 60;
  }
}

static void rfSwitchOn() {
  pinMode(PIN_RF_POWER, OUTPUT);
  digitalWrite(PIN_RF_POWER, LOW);
  pinMode(PIN_RF_SELECT, OUTPUT);
  digitalWrite(PIN_RF_SELECT, LOW);  // built-in antenna
}

// ---- ESP-NOW to the gateway ----------------------------------------------
static const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static volatile bool ackGot = false;
static LinkAck ackPkt;
static uint32_t ackWantSeq = 0, ackWantBoot = 0;
static uint8_t myMac[6];

#if ESP_IDF_VERSION_MAJOR >= 5
static void onLinkRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
#else
static void onLinkRecv(const uint8_t *mac, const uint8_t *data, int len) {
#endif
  if (len != (int)sizeof(LinkAck)) return;
  LinkAck a;
  memcpy(&a, data, sizeof a);
  if (a.magic != LINK_MAGIC || a.version != LINK_VERSION || a.type != LINK_ACK) return;
  if (a.seq != ackWantSeq || a.boot != ackWantBoot || memcmp(a.dest, myMac, 6) != 0) return;
  if (a.tag != link_tag(cfg.meshKey.c_str(), &a, sizeof a)) return;
  ackPkt = a;
  ackGot = true;
}

// aliveOnly: just "still here" (Always on mode), the gateway does not upload it unless it carries changes
static bool sendViaGateway(bool aliveOnly = false) {
  lastBattery = batteryVolts();
  LinkReading r;
  memset(&r, 0, sizeof r);
  r.magic = LINK_MAGIC;
  r.version = LINK_VERSION;
  r.type = LINK_READING;
  r.boot = rtcBootId;
  r.seq = ++rtcSeq;
  r.uptime_s = (uint32_t)time(nullptr);
  r.wakes = rtcWakes;
  r.battery_mv = (uint16_t)(lastBattery * 1000.0f + 0.5f);
  r.power = cfg.power;
  r.nch = NUM_CH;
  strncpy(r.machine, cfg.machine.c_str(), sizeof r.machine - 1);
  strncpy(r.kind, cfg.kind.c_str(), sizeof r.kind - 1);
  strncpy(r.fw, FW_VERSION, sizeof r.fw - 1);
  for (int i = 0; i < NUM_CH; i++) {
    r.ch[i].enabled = st[i].en;
    strncpy(r.ch[i].name, CH_NAMES[i], sizeof r.ch[i].name);
    r.ch[i].pulses = getPulses(i);
    r.ch[i].lpp = cfg.ch[i].lpp;
  }
  // Machine running: state now and the oldest changes not confirmed yet
  uint32_t lastEv = 0;
  if (cfg.power == POWER_ALWAYS_ON) r.run |= LINK_KEEPALIVE;
  if (rtcPoweredOn) r.run |= LINK_POWERED_UP;
  if (aliveOnly) r.run |= LINK_ALIVE_ONLY;
  if (rs.en) {
    r.run |= LINK_RUN_IN_USE | (rs.on ? LINK_RUN_ACTIVE : 0);
    portENTER_CRITICAL(&runMux);
    r.nev = rtcEvCount < LINK_MAX_EV ? rtcEvCount : LINK_MAX_EV;
    memcpy(r.ev, rtcEv, sizeof(LinkRunEvent) * r.nev);
    if (r.nev) lastEv = r.ev[r.nev - 1].n;
    g_runChanged = false;  // these go now; a newer change sets it again
    portEXIT_CRITICAL(&runMux);
  }
  r.tag = link_tag(cfg.meshKey.c_str(), &r, sizeof r);

  rfSwitchOn();
  WiFi.mode(WIFI_STA);
  esp_read_mac(myMac, ESP_MAC_WIFI_STA);
  ackWantSeq = r.seq;
  ackWantBoot = r.boot;
  ackGot = false;
  if (esp_now_init() != ESP_OK) {
    lastSendMsg = "ESP-NOW error";
    WiFi.mode(WIFI_OFF);
    return false;
  }
  esp_now_register_recv_cb(onLinkRecv);
  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof peer);
  memcpy(peer.peer_addr, BROADCAST, 6);
  peer.channel = 0;  // whatever channel the radio is on
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  // Last channel that worked (twice), then all the others
  uint8_t order[16];
  int n = 0;
  if (rtcLinkChannel >= 1 && rtcLinkChannel <= 13) { order[n++] = rtcLinkChannel; order[n++] = rtcLinkChannel; }
  for (uint8_t c = 1; c <= 13; c++) if (c != rtcLinkChannel) order[n++] = c;

  bool ok = false;
  uint8_t usedCh = 0;
  for (int i = 0; i < n && !ok; i++) {
    esp_wifi_set_channel(order[i], WIFI_SECOND_CHAN_NONE);
    esp_now_send(BROADCAST, (const uint8_t *)&r, sizeof r);
    uint32_t t0 = millis();
    while (!ackGot && millis() - t0 < 120) delay(2);
    if (ackGot) { ok = true; usedCh = order[i]; }
  }
  esp_now_deinit();
  WiFi.mode(WIFI_OFF);

  if (ok) {
    if (lastEv) dropRunEvents(lastEv);
    rtcLinkChannel = ackPkt.channel ? ackPkt.channel : usedCh;
    lastSendOk = true;
    lastSendOkMs = millis();
    lastSendMsg = "gateway OK (ch " + String(rtcLinkChannel) + ", " + String(ackPkt.rssi) + " dBm)";
    if (ackPkt.cmd == LINK_CMD_SETUP) {
      g_remoteSetup = true;
      Serial.println("The gateway asks for SETUP mode (firmware update)");
    }
  } else {
    rtcLinkChannel = 0;
    lastSendOk = false;
    lastSendMsg = "no answer from the gateway";
  }
  if (!aliveOnly || !ok)
    Serial.printf("%s %lu (%u machine on/off change(s)) -> %s\n", aliveOnly ? "Still-here" : "Reading",
                  (unsigned long)r.seq, (unsigned)r.nev, lastSendMsg.c_str());
  return ok;
}

// Settings are enough to send?
static bool linkReady() { return cfg.configured; }

// Battery mode: send once, switch the radio off
static void sendCycle() {
  bool ok = sendViaGateway();
  saveCounts(true);
  scheduleNext(ok);
}

// ----------------------------------------------------------------------------
// Deep sleep
// ----------------------------------------------------------------------------
static void goToSleep() {
  stopTickTimer();
  saveCounts(false);
  uint64_t lowMask = 0, highMask = 0;
  for (int i = 0; i < NUM_CH; i++) {
    rtcPulses[i] = getPulses(i);
    rtcLevel[i] = st[i].stable;
    if (!st[i].en) continue;
    // Wake on the next change: open contact -> wake on LOW, closed -> wake on HIGH
    if (st[i].stable) lowMask |= 1ULL << st[i].pin;
    else highMask |= 1ULL << st[i].pin;
    // The C6 has no separate RTC peripheral power domain: the pull-up is kept
    // through deep sleep by holding the pad configuration.
    if (cfg.ch[i].mode == MODE_PULLUP) gpio_pullup_en(st[i].pin);
    else gpio_pullup_dis(st[i].pin);
    gpio_pulldown_dis(st[i].pin);
    gpio_hold_en(st[i].pin);
  }
  rtcRunOn = rs.on;
  if (rs.en) {
    // Wake when the input leaves the accepted state
    uint8_t lvlNow = rs.activeLow ? !rs.on : rs.on;  // pin level of the accepted state
    if (lvlNow) lowMask |= 1ULL << rs.pin;
    else highMask |= 1ULL << rs.pin;
    if (rs.activeLow) { gpio_pullup_en(rs.pin); gpio_pulldown_dis(rs.pin); }
    else { gpio_pulldown_en(rs.pin); gpio_pullup_dis(rs.pin); }
    gpio_hold_en(rs.pin);
  }
#if SOC_PM_SUPPORT_EXT1_WAKEUP && SOC_PM_SUPPORT_EXT1_WAKEUP_MODE_PER_PIN && \
    ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
  // ESP32 core 3.1 and newer: EXT1 with a wake level per pin (bits set in the level mask wake on HIGH)
  if (lowMask | highMask) esp_sleep_enable_ext1_wakeup_with_level_mask(lowMask | highMask, highMask);
#elif SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
  // ESP32 core 3.0: GPIO deep-sleep wake-up, one level per call
  if (lowMask) esp_deep_sleep_enable_gpio_wakeup(lowMask, ESP_GPIO_WAKEUP_GPIO_LOW);
  if (highMask) esp_deep_sleep_enable_gpio_wakeup(highMask, ESP_GPIO_WAKEUP_GPIO_HIGH);
#else
#error "Update the esp32 boards package (Tools > Board > Boards Manager > esp32) to version 3.3 or newer"
#endif
  time_t now = time(nullptr);
  time_t wait = rtcNextSend > now ? rtcNextSend - now : 1;
  esp_sleep_enable_timer_wakeup((uint64_t)wait * 1000000ULL);
  led(false);
  Serial.printf("Sleeping %ld s (C=%llu W=%llu H=%llu)\n", (long)wait,
                (unsigned long long)rtcPulses[0], (unsigned long long)rtcPulses[1], (unsigned long long)rtcPulses[2]);
  Serial.flush();
  esp_deep_sleep_start();
}

// Woken from deep sleep by a contact or by the send timer: count, maybe send, sleep again
static void handleWake(esp_sleep_wakeup_cause_t cause) {
  rtcWakes++;
  setupInputs();
  delayMicroseconds(200);
  uint64_t woke = 0;
  if (cause == ESP_SLEEP_WAKEUP_EXT1) woke = esp_sleep_get_ext1_wakeup_status();
#if SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP && ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 3, 0)
  if (cause == ESP_SLEEP_WAKEUP_GPIO) woke = esp_sleep_get_gpio_wakeup_status();
#endif
  uint32_t now = millis();
  for (int i = 0; i < NUM_CH; i++) {
    ChState &c = st[i];
    if (!c.en) continue;
    uint8_t prev = rtcLevel[i];
    uint8_t lvl = gpio_get_level(c.pin) ? 1 : 0;
    bool fired = woke & (1ULL << c.pin);
    // open -> closed is a pulse. If the pin woke us but is back where it was,
    // the contact moved twice before we could look: that is also one pulse.
    if ((prev == 1 && lvl == 0) || (fired && lvl == prev)) { c.pulses++; c.lastPulseMs = now; }
    c.stable = lvl;
  }
  // Watch the contacts until they are steady, so a bounce does not wake us again
  // (and the running input until its filter has decided)
  g_debounce = cfg.debounceMs;
  uint32_t t0 = millis();
  uint32_t quiet = 0;
  uint32_t limit = 1000;
  if (rs.en && cfg.runFilterMs + 200 > limit) limit = cfg.runFilterMs + 200;
  while (millis() - t0 < limit) {
    tickOnce();
    bool busy = rs.en && rs.run;
    for (int i = 0; i < NUM_CH; i++) if (st[i].en && st[i].run) busy = true;
    quiet = busy ? 0 : quiet + 1;
    if (quiet >= cfg.debounceMs) break;
    delay(1);
  }
  for (int i = 0; i < NUM_CH; i++) rtcPulses[i] = getPulses(i);

  if (pulsesSinceSave() >= SAVE_EVERY_PULSES) saveCounts(false);
  // A start / stop goes out at once (unless the gateway was not answering: then at the next retry)
  bool runNow = g_runChanged && rtcFails == 0;
  if ((time(nullptr) >= rtcNextSend || runNow) && linkReady()) {
    startTickTimer();  // keep counting while the Wi-Fi is on
    sendCycle();
    if (g_remoteSetup) {  // stay awake in SETUP: restart into it
      stopTickTimer();
      saveCounts(true);
      rtcOpenSetup = 1;
      esp_restart();
    }
  }
  goToSleep();
}

// ----------------------------------------------------------------------------
// Settings page (awake only)
// ----------------------------------------------------------------------------
static bool configMode = false;
static uint32_t configStartMs = 0;
static WebServer server(80);
static DNSServer dns;
static String apName;
static volatile bool sendRequested = false;
static uint32_t testWindowStartMs = 0;

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
  if (configMode) return true;
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
</style></head><body>)HTML";

static String opt(const String &v, const String &label, bool sel) {
  return "<option value='" + v + "'" + (sel ? " selected" : "") + ">" + label + "</option>";
}

static void handleRoot() {
  if (!checkAuth()) return;
  String h;
  h.reserve(9000);
  h += FPSTR(PAGE_HEAD);
  h += "<h1>Water meter &middot; " + esc(cfg.machine) + "</h1>";
  h += "<div class='muted'>XIAO ESP32-C6 &middot; firmware " FW_VERSION " &middot; " + macString() + "</div>";

  h += "<h2>Current readings</h2><table><tr><th>Inlet</th><th>Pin</th><th>Contact</th><th>Pulses</th><th>Litres</th></tr>";
  for (int i = 0; i < NUM_CH; i++) {
    int pi = pinIndex(cfg.ch[i].pin);
    h += "<tr><td>" + String(CH_NAMES[i]) + (st[i].en ? "" : " (off)") + "</td><td>" + String(pi >= 0 ? PIN_LABELS[pi] : "?") +
         "</td><td>" + (st[i].en ? (st[i].stable ? "open" : "closed") : "-") + "</td><td>" +
         String((unsigned long long)getPulses(i)) + "</td><td>" + String(litres(i), 1) + "</td></tr>";
  }
  if (rs.en) {
    int ri = runPinIndex(cfg.runPin);
    h += "<tr><td>Machine running</td><td>" + String(ri >= 0 ? RUN_PIN_LABELS[ri] : "?") + "</td><td>" +
         (gpio_get_level(rs.pin) ? "open" : "closed") + "</td><td colspan='2'><b>" + (rs.on ? "RUNNING" : "stopped") +
         "</b> &middot; " + String(rtcEvCount) + " change(s) to send</td></tr>";
  }
  float bv = batteryVolts();
  h += "</table><div class='muted'>Battery: " + (bv > 0 ? String(bv, 2) + " V" : String("no divider")) +
       " &middot; last send: " + esc(lastSendMsg) + "</div>";
  if (cfg.power == POWER_BATTERY && !configMode) {
    uint32_t left = TEST_WINDOW_MS - umin(TEST_WINDOW_MS, millis() - testWindowStartMs);
    h += "<div class='muted'>Goes to deep sleep in " + String(left / 60000) + " min " + String((left / 1000) % 60) + " s. Refresh to update.</div>";
  }

  h += "<form method='post' action='/save'>";
  h += "<h2>Machine</h2><fieldset><legend>General</legend>";
  h += "<label>Machine name</label><input name='mach' maxlength='32' value='" + esc(cfg.machine) + "'>";
  h += "<label>Type</label><select name='kind'>" + opt("washer", "Washing machine", cfg.kind == "washer") +
       opt("boiler", "Boiler", cfg.kind == "boiler") + opt("canteen", "Canteen", cfg.kind == "canteen") +
       opt("other", "Other", cfg.kind == "other") + "</select>";
  h += "<label>Network key (the same on the Atom gateway)</label><input name='mkey' maxlength='32' value='" + esc(cfg.meshKey) + "'>";
  h += "<div class='row'><div><label>Send every (min)</label><input name='int' type='number' min='1' max='1440' value='" + String(cfg.intervalMin) + "'></div>";
  h += "<div><label>Debounce filter (ms)</label><input name='deb' type='number' min='2' max='500' value='" + String(cfg.debounceMs) + "'></div></div>";
  h += "<label>Power mode</label><select name='pwr'>" +
       opt("0", "Battery: deep sleep between pulses (10 min awake after reset)", cfg.power == POWER_BATTERY) +
       opt("1", "Always on (USB power, page always available)", cfg.power == POWER_ALWAYS_ON) + "</select>";
  h += "<label>Password for this page, user admin (blank = keep)</label><input name='adm' type='password'>";
  h += "</fieldset>";

  h += "<h2>Water inlets</h2>";
  for (int i = 0; i < NUM_CH; i++) {
    String p = "c" + String(i);
    h += "<fieldset><legend>" + String(CH_NAMES[i]) + "</legend>";
    h += "<label><input type='checkbox' name='" + p + "en' value='1'" + (cfg.ch[i].enabled ? " checked" : "") + "> Inlet in use</label>";
    h += "<div class='row'><div><label>Pin</label><select name='" + p + "pin'>";
    for (size_t a = 0; a < sizeof ALLOWED_PINS; a++)
      h += opt(String(ALLOWED_PINS[a]), PIN_LABELS[a], cfg.ch[i].pin == ALLOWED_PINS[a]);
    h += "</select></div><div><label>Litres per pulse</label><input name='" + p + "lpp' type='number' step='any' min='0.0001' value='" + String(cfg.ch[i].lpp, 4) + "'></div></div>";
    h += "<label>Wiring</label><select name='" + p + "mod'>";
    h += opt("0", "No pull-up (advanced)", cfg.ch[i].mode == MODE_PARALLEL_7111);
    h += opt("1", "Internal pull-up: diode to the 7111, optocoupler or bare sensor", cfg.ch[i].mode == MODE_PULLUP);
    h += "</select></fieldset>";
  }
  h += "<h2>Machine running signal</h2><fieldset><legend>Running input</legend>";
  h += "<label><input type='checkbox' name='ren' value='1'" + String(cfg.runEnabled ? " checked" : "") + "> Input in use</label>";
  h += "<div class='row'><div><label>Pin</label><select name='rpin'>";
  for (size_t a = 0; a < sizeof RUN_PINS; a++)
    h += opt(String(RUN_PINS[a]), String(RUN_PIN_LABELS[a]) + (pinAllowed(RUN_PINS[a]) ? "" : " (Always on only)"), cfg.runPin == RUN_PINS[a]);
  h += "</select></div><div><label>Filter (ms)</label><input name='rflt' type='number' min='" + String(RUN_FILTER_MIN_MS) +
       "' max='" + String(RUN_FILTER_MAX_MS) + "' value='" + String(cfg.runFilterMs) + "'></div></div>";
  h += "<label>The machine is running when the input is</label><select name='rlow'>" +
       opt("1", "Closed to GND: relay contact between the pin and GND (internal pull-up)", cfg.runActiveLow) +
       opt("0", "At 3.3 V: a signal that brings 3.3 V (internal pull-down)", !cfg.runActiveLow) + "</select>";
  h += "<div class='muted'>The pins take 3.3 V at most: never 5 V, 24 V or 230 V. A dry relay contact goes straight between the pin and GND. "
       "A change must hold for the filter time to count.</div></fieldset>";
  h += "<button type='submit'>Save and restart</button></form>";

  h += "<h2>Match the count to the 7111</h2><form method='post' action='/counts'><fieldset><legend>Current pulses</legend>";
  h += "<div class='muted'>Type the number shown on each 7111 counter. Blank = leave as is.</div>";
  for (int i = 0; i < NUM_CH; i++) {
    h += "<label>" + String(CH_NAMES[i]) + " (now: " + String((unsigned long long)getPulses(i)) + ")</label><input name='p" + String(i) + "' type='number' min='0' step='1'>";
  }
  h += "</fieldset><button type='submit'>Save count</button></form>";

  h += "<h2>Test</h2><form method='post' action='/send'><button type='submit'>Send a reading now</button></form>";
  h += "<h2>Firmware</h2><div class='muted'>Now " FW_VERSION ". <a href='/update'>Update over Wi-Fi</a> (asks for the page password).</div>";
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
  n.kind = server.arg("kind");
  if (n.kind != "washer" && n.kind != "boiler" && n.kind != "canteen" && n.kind != "other") n.kind = "washer";
  n.intervalMin = constrain(server.arg("int").toInt(), 1, 1440);
  n.debounceMs = constrain(server.arg("deb").toInt(), 2, 500);
  n.power = server.arg("pwr").toInt() == 1 ? POWER_ALWAYS_ON : POWER_BATTERY;
  n.meshKey = server.arg("mkey"); n.meshKey.trim();
  if (n.meshKey.isEmpty()) n.meshKey = MESH_KEY;
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
  n.runEnabled = server.hasArg("ren");
  n.runPin = (uint8_t)server.arg("rpin").toInt();
  n.runActiveLow = server.arg("rlow").toInt() != 0;
  n.runFilterMs = constrain(server.arg("rflt").toInt(), RUN_FILTER_MIN_MS, RUN_FILTER_MAX_MS);
  if (n.runEnabled) {
    if (runPinIndex(n.runPin) < 0) err += "Invalid pin for the running input. ";
    else if (!runPinOk(n.runPin, n.power)) err += "In Battery mode the running input must be on D0-D2 or a G5/G6/G7 pad (only these wake the chip). ";
    for (int i = 0; i < NUM_CH; i++)
      if (n.ch[i].enabled && n.ch[i].pin == n.runPin) err += String("The running input and ") + CH_NAMES[i] + " are on the same pin. ";
  }
  if (err.length()) { sendMessagePage("Nothing was saved", esc(err), true); return; }

  cfg = n;
  saveConfig();
  saveCounts(true);
  sendMessagePage("Saved", "Restarting. The yellow LED blinks for 3 s, then the XIAO sends a first reading (after about 3 s). Short press on BOOT sends again.", false);
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
  if (configMode) { sendMessagePage("SETUP mode", "To send, save the settings first. After the restart, a short press on BOOT sends a reading.", true); return; }
  sendRequested = true;
  sendMessagePage("Sending", "Send requested. Refresh the main page in a few seconds to see the result.", true);
}

static void handleExit() {
  saveCounts(true);
  sendMessagePage("Leaving", "Restarting in normal mode.", false);
  delay(1000);
  ESP.restart();
}

// ---- Firmware update (SETUP page) -----------------------------------------
// Always asks for the page password (user admin), even in SETUP mode.
static bool otaAllowed = false;
static String otaMsg;

static void handleUpdatePage() {
  if (!server.authenticate("admin", cfg.adminPass.c_str())) { server.requestAuthentication(BASIC_AUTH, "Water meter"); return; }
  String h = FPSTR(PAGE_HEAD);
  h += "<h1>Firmware update</h1><div class='muted'>Now: " FW_VERSION " &middot; " + esc(cfg.machine) + "</div>";
  h += "<p>Choose the <b>.bin</b> made by Arduino IDE (Sketch &gt; Export Compiled Binary, board XIAO_ESP32C6). "
       "Settings and counts are kept. Takes about 30 s, then the XIAO restarts.</p>";
  h += "<form method='post' action='/update' enctype='multipart/form-data'><input type='file' name='fw' accept='.bin' required>"
       "<button type='submit'>Update</button></form><p><a href='/'>Back</a></p></body></html>";
  server.send(200, "text/html; charset=utf-8", h);
}

static void handleUpdateUpload() {
  HTTPUpload &u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    otaAllowed = server.authenticate("admin", cfg.adminPass.c_str());
    otaMsg = "";
    if (!otaAllowed) return;
    saveCounts(true);
    Serial.printf("Firmware update: %s\n", u.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) otaMsg = Update.errorString();
  } else if (!otaAllowed || otaMsg.length()) {
    if (u.status == UPLOAD_FILE_ABORTED) Update.abort();
    return;
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (Update.write(u.buf, u.currentSize) != u.currentSize) otaMsg = Update.errorString();
  } else if (u.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) otaMsg = Update.errorString();
    else Serial.printf("Firmware update: %u bytes OK\n", (unsigned)u.totalSize);
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaMsg = "upload cancelled";
  }
}

static void handleUpdateDone() {
  if (!otaAllowed) { server.requestAuthentication(BASIC_AUTH, "Water meter"); return; }
  if (otaMsg.length() || !Update.isFinished()) {
    sendMessagePage("Update failed", "Nothing was changed: " + esc(otaMsg.length() ? otaMsg : String("incomplete file")), true);
    return;
  }
  sendMessagePage("Updated", "Restarting with the new firmware. The counts are kept.", false);
  delay(1500);
  ESP.restart();
}

static void setupServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/counts", HTTP_POST, handleCounts);
  server.on("/send", HTTP_POST, handleSendNow);
  server.on("/exit", HTTP_POST, handleExit);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.onNotFound([]() {
    if (configMode) {
      server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
      server.send(302);
    } else {
      server.send(404, "text/plain", "Not found");
    }
  });
  server.begin();
}

static void startConfigMode() {
  configMode = true;
  configStartMs = millis();
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char buf[24];
  snprintf(buf, sizeof buf, "WaterMeter-%02X%02X", mac[4], mac[5]);
  apName = buf;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apName.c_str(), AP_PASSWORD);
  dns.start(53, "*", WiFi.softAPIP());
  WiFi.scanNetworks(true);
  Serial.printf("SETUP mode: Wi-Fi %s password %s -> http://192.168.4.1/\n", apName.c_str(), AP_PASSWORD);
}

// ----------------------------------------------------------------------------
// setup / loop
// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool fromSleep = rtcMagic == RTC_MAGIC &&
                   (cause == ESP_SLEEP_WAKEUP_EXT1 || cause == ESP_SLEEP_WAKEUP_GPIO || cause == ESP_SLEEP_WAKEUP_TIMER);
  loadConfig();

  if (fromSleep && cfg.power == POWER_BATTERY) {
    pinMode(PIN_LED, OUTPUT);
    led(false);
    handleWake(cause);  // does not return
  }

  // SETUP asked by the gateway before this restart (battery mode)
  bool remoteSetup = rtcMagic == RTC_MAGIC && rtcOpenSetup == 1;
  rtcOpenSetup = 0;
  // Power-up, RESET button or restart: counts come back from flash
  if (rtcMagic != RTC_MAGIC || !fromSleep) {
    esp_reset_reason_t rr = esp_reset_reason();
    // From no power (machine switched on) or the RESET button; not a restart after settings / update
    rtcPoweredOn = (rr == ESP_RST_POWERON || rr == ESP_RST_BROWNOUT || rr == ESP_RST_UNKNOWN || rr == ESP_RST_EXT) ? 1 : 0;
    loadCountsFromFlash();
    rtcMagic = RTC_MAGIC;
    rtcBootId = esp_random();
    rtcSeq = 0;
    rtcFails = 0;
    rtcWakes = 0;
    rtcRunN = 0;
    rtcEvCount = 0;
    rtcRunOn = 0;
  }
  delay(300);  // give the USB serial a moment
  Serial.printf("\nWater meter %s on XIAO ESP32-C6, %s\n", FW_VERSION, macString().c_str());

  pinMode(PIN_LED, OUTPUT);
  led(false);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  rfSwitchOn();

  setupInputs();
  delay(5);
  for (int i = 0; i < NUM_CH; i++) st[i].stable = st[i].en ? (gpio_get_level(st[i].pin) ? 1 : 0) : 1;
  if (rs.en) {
    // State at power-up (also marks that the reader was off before, e.g. no 5 V)
    rs.on = runRaw();
    pushRunEvent(rs.on | (rtcPoweredOn ? LINK_EV_POWERUP : 0), (uint32_t)time(nullptr));
    g_runChanged = false;  // goes with the first reading, 3 s after start-up
  }
  startTickTimer();

  // 3 s window: press BOOT for SETUP
  bool wantSetup = !linkReady() || remoteSetup;
  if (!wantSetup) {
    Serial.println("Press BOOT now (LED blinking) for SETUP mode...");
    uint32_t t0 = millis();
    while (millis() - t0 < SETUP_PRESS_WINDOW_MS) {
      led(((millis() - t0) / 100) % 2);
      if (digitalRead(PIN_BOOT) == LOW) { wantSetup = true; break; }
      delay(10);
    }
    led(false);
  }

  if (wantSetup) startConfigMode();
  else { WiFi.mode(WIFI_OFF); Serial.println("Sending to the Atom gateway (ESP-NOW). Short press BOOT to send now."); }
  if (configMode) setupServer();  // no page when the radio is off
  lastBattery = batteryVolts();
  if (lastBattery > 0) Serial.printf("Battery: %.2f V\n", lastBattery);
  else Serial.println("Battery: no divider on the MTMS pad");
  for (int i = 0; i < NUM_CH; i++)
    if (st[i].en) Serial.printf("%s on GPIO%d: contact %s\n", CH_NAMES[i], st[i].pin, st[i].stable ? "open" : "closed");
  if (rs.en) Serial.printf("Machine running input on GPIO%d: %s\n", rs.pin, rs.on ? "RUNNING" : "stopped");
  testWindowStartMs = millis();
}

void loop() {
  if (configMode) {
    dns.processNextRequest();
    led((millis() / 1000) % 2);  // slow blink in SETUP
  }
  server.handleClient();

  // LED flash and serial line on each pulse
  static uint64_t shown[NUM_CH] = {0, 0, 0};
  static bool shownInit = false;
  if (!shownInit) { for (int i = 0; i < NUM_CH; i++) shown[i] = getPulses(i); shownInit = true; }
  for (int i = 0; i < NUM_CH; i++) {
    uint64_t v = getPulses(i);
    if (v != shown[i]) {
      shown[i] = v;
      Serial.printf("Pulse %s: %llu pulses = %.1f L\n", CH_NAMES[i], (unsigned long long)v, litres(i));
    }
  }
  static uint8_t shownRun = 255;
  if (rs.en && rs.on != shownRun) {
    if (shownRun != 255) Serial.printf("Machine %s\n", rs.on ? "STARTED" : "STOPPED");
    shownRun = rs.on;
  }
  if (!configMode) led(g_lastAnyPulseMs && millis() - g_lastAnyPulseMs < 150);

  // Always-on mode: hold BOOT 5 s for SETUP
  // BOOT: short press = send now, hold 5 s = SETUP
  static uint32_t bootDownAt = 0;
  if (digitalRead(PIN_BOOT) == LOW) {
    if (!bootDownAt) bootDownAt = millis();
    if (!configMode && millis() - bootDownAt > CONFIG_LONGPRESS_MS) {
      saveCounts(true);
      WiFi.disconnect(true);
      startConfigMode();
      setupServer();
    }
  } else {
    if (bootDownAt && !configMode && millis() - bootDownAt > 50 && millis() - bootDownAt < 1000) sendRequested = true;
    bootDownAt = 0;
  }

  // The gateway asked for SETUP (to update the firmware): open it now
  if (g_remoteSetup && !configMode) {
    g_remoteSetup = false;
    saveCounts(true);
    startConfigMode();
    setupServer();
  }

  // SETUP times out
  if (configMode && linkReady() && millis() - configStartMs > CONFIG_AP_TIMEOUT_MS) {
    saveCounts(true);
    ESP.restart();
  }

  // Sending while awake
  static uint32_t connectedAt = 0;
  static uint32_t lastAttempt = 0;   // last send of any kind
  static uint32_t lastReading = 0;   // last full reading (for "Send every")
  static bool firstDone = false;
  if (!configMode) {
    if (connectedAt == 0) connectedAt = millis() - FIRST_SEND_DELAY_MS + 3000;  // first reading 3 s after start-up
    uint32_t interval = (uint32_t)cfg.intervalMin * 60000UL;
    bool due = (!firstDone && millis() - connectedAt >= FIRST_SEND_DELAY_MS) ||
               (firstDone && millis() - lastReading >= interval);
    // A start / stop goes out at once; if the gateway did not answer, retry after a minute
    bool runDue = firstDone && g_runChanged && millis() - lastAttempt >= RUN_SEND_GAP_MS;
    bool runRetry = firstDone && rtcEvCount && !lastSendOk && millis() - lastAttempt >= 60000UL;
    if (due || sendRequested || runDue || runRetry) {
      sendRequested = false;
      firstDone = true;
      lastAttempt = lastReading = millis();
      bool ok = sendViaGateway();
      // LED: 3 short blinks = the gateway got it, 1 long = no answer
      if (ok) for (int b = 0; b < 3; b++) { led(true); delay(80); led(false); delay(120); }
      else { led(true); delay(800); led(false); }
      saveCounts(true);
      scheduleNext(ok);
    } else if (cfg.power == POWER_ALWAYS_ON && firstDone && millis() - lastAttempt >= ALIVE_EVERY_MS) {
      // "Still here" every minute: when it stops, the gateway knows the machine was switched off
      lastAttempt = millis();
      sendViaGateway(true);
    }
  }

  static uint32_t lastSaveMs = 0;
  if (millis() - lastSaveMs >= SAVE_EVERY_MS) {
    lastSaveMs = millis();
    saveCounts(false);
  }

  // End of the test window: deep sleep (battery mode)
  if (cfg.power == POWER_BATTERY && !configMode && millis() - testWindowStartMs > TEST_WINDOW_MS) {
    if (!firstDone) rtcNextSend = time(nullptr) + (time_t)RETRY_MIN * 60;  // never got to send
    server.stop();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("Test window over: going to deep sleep.");
    goToSleep();
  }
  delay(2);
}
