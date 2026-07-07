// reader.h — shared reader state (LoRa side fills it, web/MQTT side reads it)
#pragma once
#include <Arduino.h>

struct Reader {
  bool     used     = false;
  uint8_t  handle[3];            // 3-byte LoRa id (frame header)
  // pairing state
  bool     assigned = false;     // user accepted this reader onto THIS gateway (persisted); else it is only
                                 // shown greyed-out and NOT bound/keyed, so two gateways don't fight over it
  bool     haveKey  = false;
  bool     isOld    = false;
  bool     decoded  = false;
  uint8_t  decFails = 0;
  uint32_t lastBind = 0;
  float    lastRssi = -70;
  float    lastSnr  = 0;         // SX1262 packet SNR (dB) of the last frame from this reader
  uint8_t  key[16];
  // identity (from the announce: cmd 17/35)
  bool     haveUuid = false;
  uint8_t  uuid[16];             // 16-byte device UUID
  uint8_t  devType  = 0x10;      // 0x10 meter, 0x11 outlet
  // latest telemetry (from the energy frame)
  bool     haveData = false;
  uint8_t  softver = 0, hardver = 0;
  uint16_t battery_mV = 0;
  uint8_t  flags = 0;            // b0 infrared, b1 lowpower, b2 timesync
  uint32_t import_ = 0, export_ = 0, power = 0;   // 0x7FFFFFFF = n/a
  bool     legacy  = false;      // legacy (3x.x) layout
  bool     inBootloader = false;  // reader reset into its OTA bootloader (sends cmd 20/21/33, no energy)
  uint32_t lastSeenMs = 0;
  // config
  uint16_t setInterval = 0;      // last upload-interval requested (shown in UI)
  uint8_t  intervalTx  = 0;      // remaining cmd-14 retransmits to send
  char     name[25] = {0};       // user-set friendly name ("" = unset); persisted in NVS ns "obiname"
  bool     mqttDiscovered = false;  // HA discovery config already published (reset on each MQTT connect)
};

// defined in main.cpp
extern Reader readers[];
extern const int MAX_READERS;
extern const uint8_t GWID[6];

// LoRa-side actions the web UI triggers (defined in main.cpp)
void gw_request_interval(const uint8_t handle[3], uint16_t seconds);  // set the reader's upload interval
bool gw_delete_reader(const uint8_t handle[3]);                       // drop a (phantom) reader; reappears on next RX
uint32_t gw_uptime_s();
// reader pairing gating (opt-in): a reader is only bound/keyed once assigned to this gateway
bool gw_assign_reader(const uint8_t handle[3], bool on);              // accept (or drop) a reader onto this gateway
void gw_set_reader_name(const uint8_t handle[3], const char *name);   // set ("" clears) the friendly name
void gw_pair_all(uint16_t seconds);                                   // open a window that auto-assigns every reader
uint32_t gw_pair_remaining_s();                                       // seconds left in the auto-pair window (0 = off)

// live radio log (ring buffer) for the /radio web page
void   gw_radio_log(char dir, const uint8_t h[3], int cmd, int len, int rssi, int snr, const char *note,
                    const uint8_t *raw, int rawLen);                  // dir 'R'/'T'; raw = full packet bytes
String gw_radio_json(uint32_t since);                                 // {seq, e:[...]} — entries newer than `since`

// ---- reader firmware OTA over LoRa (serve to the reader's bootloader) --------
bool     gw_ota_begin(const uint8_t handle[3], uint32_t total, uint8_t version);  // alloc + target a reader
void     gw_ota_write(const uint8_t *data, uint32_t len);                          // append an upload chunk
bool     gw_ota_arm();                                                             // finished upload -> start advertising
void     gw_ota_cancel();
// status for the dashboard
bool     gw_ota_active();
uint32_t gw_ota_size();
uint32_t gw_ota_progress();   // bytes served so far
void     gw_ota_target(uint8_t out[3]);

// helper: is an energy field "no reading"?
static inline bool obi_na(uint32_t v) { return v == 0x7FFFFFFF; }
