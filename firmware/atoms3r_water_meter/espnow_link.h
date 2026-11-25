// =============================================================================
//  ESP-NOW link between the battery meter readers (XIAO ESP32-C6) and the
//  gateway (AtomS3R). The same file is in both sketch folders: keep them equal.
//
//  - The reader broadcasts a LinkReading on the gateway's Wi-Fi channel.
//    It does not know that channel at first: it tries the last one that
//    worked, then channels 1 to 13, until the gateway answers.
//  - The gateway checks the tag, answers with a LinkAck (which carries its
//    channel) and sends the reading to Supabase over its own Wi-Fi.
//  - Each packet carries a 64-bit SipHash-2-4 tag made with the network key
//    (the same text on both sides), so other ESP-NOW devices nearby cannot
//    add readings.
// =============================================================================
#pragma once
#include <stdint.h>
#include <string.h>

#define LINK_MAGIC 0x4D57      // "WM"
#define LINK_VERSION 1
#define LINK_MAX_CH 3
#ifndef MESH_KEY
#define MESH_KEY "autometers-net"  // default network key (override in secrets.h or on the settings page)
#endif

enum : uint8_t { LINK_READING = 1, LINK_ACK = 2 };

struct __attribute__((packed)) LinkChannel {
  uint8_t enabled;
  char name[8];
  uint64_t pulses;
  float lpp;
};

struct __attribute__((packed)) LinkReading {
  uint16_t magic;
  uint8_t version;
  uint8_t type;        // LINK_READING
  uint32_t boot;       // random number chosen at power-up
  uint32_t seq;        // reading number since power-up
  uint32_t uptime_s;
  uint32_t wakes;
  uint16_t battery_mv; // 0 = not measured
  uint8_t power;       // 0 = battery, 1 = always on
  uint8_t nch;
  char machine[33];
  char kind[9];
  char fw[16];
  LinkChannel ch[LINK_MAX_CH];
  uint64_t tag;        // SipHash of everything above
};

struct __attribute__((packed)) LinkAck {
  uint16_t magic;
  uint8_t version;
  uint8_t type;        // LINK_ACK
  uint32_t boot;       // copied from the reading
  uint32_t seq;        // copied from the reading
  uint8_t channel;     // gateway's Wi-Fi channel
  int8_t rssi;         // how the gateway heard the reader
  uint8_t dest[6];     // MAC of the reader this answer is for
  uint64_t tag;
};

// What the gateway keeps for each packet it hears (defined here so that the
// Arduino IDE's automatic prototypes can see it)
struct RxItem {
  uint8_t mac[6];
  int8_t rssi;
  LinkReading r;
};

// ---- SipHash-2-4 ------------------------------------------------------------
static inline uint64_t link_rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }
#define LINK_SIPROUND                                              \
  do {                                                             \
    v0 += v1; v1 = link_rotl(v1, 13); v1 ^= v0; v0 = link_rotl(v0, 32); \
    v2 += v3; v3 = link_rotl(v3, 16); v3 ^= v2;                    \
    v0 += v3; v3 = link_rotl(v3, 21); v3 ^= v0;                    \
    v2 += v1; v1 = link_rotl(v1, 17); v1 ^= v2; v2 = link_rotl(v2, 32); \
  } while (0)

static inline uint64_t link_siphash(uint64_t k0, uint64_t k1, const uint8_t *in, size_t len) {
  uint64_t v0 = 0x736f6d6570736575ULL ^ k0, v1 = 0x646f72616e646f6dULL ^ k1;
  uint64_t v2 = 0x6c7967656e657261ULL ^ k0, v3 = 0x7465646279746573ULL ^ k1;
  size_t full = len & ~(size_t)7;
  for (size_t i = 0; i < full; i += 8) {
    uint64_t m;
    memcpy(&m, in + i, 8);
    v3 ^= m; LINK_SIPROUND; LINK_SIPROUND; v0 ^= m;
  }
  uint64_t b = (uint64_t)len << 56;
  for (size_t i = 0; i < (len & 7); i++) b |= (uint64_t)in[full + i] << (8 * i);
  v3 ^= b; LINK_SIPROUND; LINK_SIPROUND; v0 ^= b;
  v2 ^= 0xff;
  LINK_SIPROUND; LINK_SIPROUND; LINK_SIPROUND; LINK_SIPROUND;
  return v0 ^ v1 ^ v2 ^ v3;
}

// Tag for a packet whose last 8 bytes are the tag itself
static inline uint64_t link_tag(const char *key, const void *pkt, size_t len) {
  size_t kl = strlen(key);
  uint64_t k0 = link_siphash(0x0706050403020100ULL, 0x0f0e0d0c0b0a0908ULL, (const uint8_t *)key, kl);
  uint64_t k1 = link_siphash(0x1716151413121110ULL, 0x1f1e1d1c1b1a1918ULL, (const uint8_t *)key, kl);
  return link_siphash(k0, k1, (const uint8_t *)pkt, len - 8);
}
