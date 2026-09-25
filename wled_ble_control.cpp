/*
 * WLED BLE Control usermod
 *
 * Exposes a BLE GATT service so the WledBleWear WearOS app can read and
 * control power, brightness, presets and the nightlight. The wire protocol is
 * specified in docs/PROTOCOL.md; keep the two in step.
 *
 * Target: WLED 16.x, ESP32 / ESP32-C3.
 *
 * Install as a standalone usermod:
 *   custom_usermods =
 *     symlink:///path/to/WLED-BLE-Control-Usermod
 */

#include "wled.h"

#ifndef ARDUINO_ARCH_ESP32
  #error "wled-ble-control needs an ESP32 with Bluetooth LE (not ESP8266 or ESP32-S2)"
#endif

#if !defined(WLED_USE_SHARED_RMT) && !defined(__riscv)
  // tools/build_flags.py sets this; RMT-HI + BT controller = interrupt watchdog reset.
  #error "wled-ble-control needs WLED_USE_SHARED_RMT on ESP32 (RMT-HI LED driver conflicts with the BT controller)"
#endif

#include <NimBLEDevice.h>
#include <esp_wifi.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <vector>

// Diagnostics:
//   -D BLE_DEFER_START  don't start BLE in setup(); start it when /json/state
//                       receives {"ble":{"start":true}}. A crashing init then
//                       can't make the bootloader roll back an OTA update.
//   -D BLE_DIAG         keep BLE_LOG lines in RTC memory, which survives a panic
//                       reset, and report them with the reset reason under
//                       "BLE last boot" in /json/info on the next boot.
#ifdef BLE_DIAG
  #include <esp_system.h>
  #define RTC_LOG_MAGIC 0xB1E10601u
  static RTC_NOINIT_ATTR uint32_t rtcLogMagic;
  static RTC_NOINIT_ATTR char rtcLog[600];
  static void bleLog(const char *fmt, ...) {
    if (rtcLogMagic != RTC_LOG_MAGIC) return;
    char buf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    char line[180];
    int n = snprintf(line, sizeof(line), "%lu %s|", millis(), buf);
    size_t used = strnlen(rtcLog, sizeof(rtcLog) - 1);
    if (used + n >= sizeof(rtcLog)) {  // keep the newest entries
      const char *keep = strchr(rtcLog + used / 2, '|');
      keep = keep ? keep + 1 : rtcLog + used;
      memmove(rtcLog, keep, strlen(keep) + 1);
      used = strlen(rtcLog);
    }
    strlcpy(rtcLog + used, line, sizeof(rtcLog) - used);
  }
  #define BLE_LOG(...) bleLog(__VA_ARGS__)
#else
  #define BLE_LOG(fmt, ...) DEBUG_PRINTF(fmt "\n", ##__VA_ARGS__)
#endif

namespace WLEDBle {

// ---- Protocol constants (docs/PROTOCOL.md) ---------------------------------

// Base UUID 8e59xxxx-14dc-405e-9f47-e363750a861a
static const char *SERVICE_UUID     = "8e590001-14dc-405e-9f47-e363750a861a";
static const char *CHAR_INFO_UUID   = "8e590002-14dc-405e-9f47-e363750a861a";
static const char *CHAR_STATE_UUID  = "8e590003-14dc-405e-9f47-e363750a861a";
static const char *CHAR_CMD_UUID    = "8e590004-14dc-405e-9f47-e363750a861a";
static const char *CHAR_PRESET_UUID = "8e590005-14dc-405e-9f47-e363750a861a";

static constexpr uint8_t  PROTOCOL_VERSION = 1;
static constexpr uint16_t FEATURES = 0x001F;  // brightness, presets, nightlight, playlists, off+disconnect
static constexpr uint8_t  INFO_STATUS_PAIRING = 0x01;

enum Opcode : uint8_t {
  OP_POWER            = 0x01,
  OP_BRIGHTNESS_SET   = 0x02,
  OP_BRIGHTNESS_STEP  = 0x03,
  OP_PRESET_APPLY     = 0x04,
  OP_PRESET_NEXT      = 0x05,
  OP_PRESET_PREV      = 0x06,
  OP_NIGHTLIGHT       = 0x07,
  OP_OFF_DISCONNECT   = 0x08,
};

enum Status : uint8_t {
  ST_OK          = 0x00,
  ST_UNKNOWN_OP  = 0x80,
  ST_BAD_LENGTH  = 0x81,
  ST_INVALID     = 0x82,
  ST_BUSY        = 0x83,
};

struct __attribute__((packed)) StatePacket {
  uint8_t  flags;        // bit0 power on, bit1 nightlight active
  uint8_t  brightness;   // current, or the brightness it returns to while off
  uint8_t  preset;       // 0 = none
  uint8_t  playlist;     // 0 = none
  uint16_t presetRev;
  uint16_t nlRemaining;  // seconds; not a notification trigger
};
static_assert(sizeof(StatePacket) == 8, "StatePacket must be 8 bytes");
static constexpr size_t STATE_NOTIFY_BYTES = offsetof(StatePacket, nlRemaining);

static constexpr uint32_t NOTIFY_MIN_INTERVAL_MS = 250;
static constexpr uint32_t DISCONNECT_DELAY_MS    = 250;
static constexpr size_t   PRESET_PAGE_MAX        = 500;
static constexpr size_t   PRESET_NAME_MAX        = 32;
static constexpr size_t   PRESET_QL_MAX          = 8;
static constexpr uint8_t  MAX_PRESET_ID          = 250;
static constexpr size_t   MAX_PEERS              = 4;
static constexpr uint32_t PRESETS_SETTLE_MS      = 1100;
static constexpr uint32_t PRESETS_SIZE_CHECK_MS  = 10000;

// CRC-16/CCITT-FALSE
static uint16_t crc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  while (len--) {
    crc ^= (uint16_t)(*data++) << 8;
    for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
  }
  return crc;
}

// Longest prefix of s, at most maxBytes, that doesn't split a UTF-8 sequence.
static size_t utf8Prefix(const char *s, size_t maxBytes) {
  size_t len = strlen(s);
  if (len <= maxBytes) return len;
  len = maxBytes;
  while (len > 0 && ((uint8_t)s[len] & 0xC0) == 0x80) len--;
  return len;
}

static inline bool testBit(const uint8_t *bits, uint8_t id) { return bits[id >> 3] & (1 << (id & 7)); }
static inline void setBit(uint8_t *bits, uint8_t id)        { bits[id >> 3] |= (1 << (id & 7)); }

// ---- Preset list cache ------------------------------------------------------
//
// Built in loop() from presets.json, read from the BLE host task when a client
// asks for a page. The records are kept in wire format, sorted by ID:
//   [id][flags][nameLen][name][qlLen][ql]
class PresetCache {
  std::vector<uint8_t> records;
  uint8_t exists[32] = {};
  uint8_t favourite[32] = {};
  uint16_t rev = 0;
  SemaphoreHandle_t mutex = nullptr;

public:
  static constexpr uint8_t FLAG_PLAYLIST  = 0x01;
  static constexpr uint8_t FLAG_FAVOURITE = 0x02;

  void begin() { if (!mutex) mutex = xSemaphoreCreateMutex(); }

  uint16_t revision() const { return rev; }

  // Loop context only: needs WLED's JSON buffer.
  bool rebuild() {
    if (!requestJSONBufferLock(JSON_LOCK_PRESET_NAME)) return false;

    StaticJsonDocument<128> filter;
    filter["*"]["n"] = true;
    filter["*"]["ql"] = true;
    filter["*"]["playlist"]["end"] = true;  // only its presence matters

    std::vector<uint8_t> newRecords;
    uint8_t newExists[32] = {};
    uint8_t newFavourite[32] = {};

    if (readObjectFromFile(getPresetsFileName(), nullptr, pDoc, &filter)) {
      JsonObject root = pDoc->as<JsonObject>();
      for (JsonPair kv : root) {
        int id = atoi(kv.key().c_str());
        if (id >= 1 && id <= MAX_PRESET_ID) setBit(newExists, id);
      }
      char key[4];
      for (int id = 1; id <= MAX_PRESET_ID; id++) {
        if (!testBit(newExists, id)) continue;
        snprintf(key, sizeof(key), "%d", id);
        JsonObject p = root[key];

        char fallback[12];
        const char *name = p["n"] | "";
        if (!*name) { snprintf(fallback, sizeof(fallback), "Preset %d", id); name = fallback; }
        const char *ql = p["ql"] | "";

        uint8_t flags = 0;
        if (p["playlist"].is<JsonObject>()) flags |= FLAG_PLAYLIST;
        if (*ql) { flags |= FLAG_FAVOURITE; setBit(newFavourite, id); }

        size_t nameLen = utf8Prefix(name, PRESET_NAME_MAX);
        size_t qlLen = utf8Prefix(ql, PRESET_QL_MAX);
        newRecords.push_back(id);
        newRecords.push_back(flags);
        newRecords.push_back(nameLen);
        newRecords.insert(newRecords.end(), name, name + nameLen);
        newRecords.push_back(qlLen);
        newRecords.insert(newRecords.end(), ql, ql + qlLen);
      }
    }
    releaseJSONBufferLock();

    uint16_t newRev = crc16(newRecords.data(), newRecords.size());
    xSemaphoreTake(mutex, portMAX_DELAY);
    records.swap(newRecords);
    memcpy(exists, newExists, sizeof(exists));
    memcpy(favourite, newFavourite, sizeof(favourite));
    rev = newRev;
    xSemaphoreGive(mutex);
    BLE_LOG("BLE: preset cache %u bytes, rev %04x", records.size(), rev);
    return true;
  }

  bool has(uint8_t id) const { return id >= 1 && id <= MAX_PRESET_ID && testBit(exists, id); }

  // Next/previous existing preset after `from`, wrapping round. 0 if none.
  uint8_t step(uint8_t from, bool favouritesOnly, bool forward) const {
    const uint8_t *bits = favouritesOnly ? favourite : exists;
    for (int i = 1; i <= MAX_PRESET_ID; i++) {
      int id = forward ? from + i : from - i;
      id = ((id - 1) % MAX_PRESET_ID + MAX_PRESET_ID) % MAX_PRESET_ID + 1;  // wrap into 1..250
      if (testBit(bits, id)) return id;
    }
    return 0;
  }

  // Any context. Writes one page (docs/PROTOCOL.md) and returns its length.
  size_t page(uint8_t startId, uint8_t *out, size_t maxLen) const {
    xSemaphoreTake(mutex, portMAX_DELAY);
    size_t used = 4, pos = 0;
    uint8_t count = 0, nextId = 0;
    while (pos < records.size()) {
      uint8_t id = records[pos];
      size_t nameLen = records[pos + 2];
      size_t recLen = 3 + nameLen + 1 + records[pos + 3 + nameLen];
      if (id >= startId) {
        if (used + recLen > maxLen) { nextId = id; break; }
        memcpy(out + used, &records[pos], recLen);
        used += recLen;
        count++;
      }
      pos += recLen;
    }
    out[0] = rev & 0xFF;
    out[1] = rev >> 8;
    out[2] = nextId;
    out[3] = count;
    xSemaphoreGive(mutex);
    return used;
  }
};

// ---- Usermod ----------------------------------------------------------------

struct Command {
  uint8_t  op;
  int16_t  arg;
  uint16_t connHandle;
};

struct Peer {
  uint16_t handle = BLE_HS_CONN_HANDLE_NONE;
  bool stateSub = false;
  bool cmdSub = false;
};

class BleControlUsermod : public Usermod,
                          public NimBLEServerCallbacks,
                          public NimBLECharacteristicCallbacks {
  // Settings
  bool enabled = true;
  uint32_t passkey = 0;         // 6 digits; generated on first boot
  uint16_t pairWindowSec = 120;

  // BLE objects
  bool initDone = false;
  NimBLEServer *server = nullptr;
  NimBLECharacteristic *infoChar = nullptr;
  NimBLECharacteristic *stateChar = nullptr;
  NimBLECharacteristic *cmdChar = nullptr;
  NimBLECharacteristic *presetChar = nullptr;

  QueueHandle_t cmdQueue = nullptr;
  PresetCache presets;
  unsigned long presetsSeenTime = 0;
  size_t presetsSeenSize = 0;
  uint32_t presetsChangedMs = 0;
  uint32_t presetsSizeCheckMs = 0;
  bool presetsDirty = true;

  // Peers, touched from both the host task and loop()
  Peer peers[MAX_PEERS];
  portMUX_TYPE peersMux = portMUX_INITIALIZER_UNLOCKED;

  StatePacket lastNotified = {};
  StatePacket lastValue = {};
  bool notifyPending = false;
  uint32_t lastNotifyMs = 0;

  bool pairingOpen = false;
  uint32_t pairingOpenedMs = 0;
  volatile bool pairRequested = false;
  volatile bool forgetRequested = false;
  bool startRequested = false;   // BLE_DEFER_START

  uint32_t advCheckMs = 0;

  uint16_t pendingDisconnect = BLE_HS_CONN_HANDLE_NONE;
  uint32_t disconnectAtMs = 0;

  uint32_t heapBeforeInit = 0, heapAfterInit = 0;
#ifdef BLE_DIAG
  String lastBootLog;
  int lastResetReason = -1;
#endif

  static const char _name[];

  // ---- State -----------------------------------------------------------------

  StatePacket readState() const {
    StatePacket s = {};
    if (bri > 0) s.flags |= 0x01;
    if (nightlightActive) s.flags |= 0x02;
    s.brightness = bri > 0 ? bri : briLast;
    s.preset = currentPreset;
    s.playlist = currentPlaylist > 0 ? (uint8_t)currentPlaylist : 0;
    s.presetRev = presets.revision();
    if (nightlightActive) {
      uint32_t totalMs = nightlightActiveOld ? nightlightDelayMs : nightlightDelayMins * 60000UL;
      uint32_t elapsed = nightlightActiveOld ? millis() - nightlightStartTime : 0;
      s.nlRemaining = elapsed < totalMs ? (totalMs - elapsed + 999) / 1000 : 0;
    }
    return s;
  }

  void updateState() {
    StatePacket s = readState();
    if (memcmp(&s, &lastValue, sizeof(s)) != 0) {
      lastValue = s;
      stateChar->setValue((uint8_t *)&s, sizeof(s));
    }
    if (memcmp(&s, &lastNotified, STATE_NOTIFY_BYTES) != 0) notifyPending = true;
    if (notifyPending && millis() - lastNotifyMs >= NOTIFY_MIN_INTERVAL_MS) {
      notifyPending = false;
      lastNotifyMs = millis();
      lastNotified = s;
      notifyPeers(stateChar, (uint8_t *)&s, sizeof(s), true);
    }
  }

  // Notify only peers that subscribed and hold an authenticated link. NimBLE's
  // notify() with a value would send to every connected peer.
  void notifyPeers(NimBLECharacteristic *chr, const uint8_t *data, size_t len, bool stateChannel, uint16_t onlyHandle = BLE_HS_CONN_HANDLE_NONE) {
    uint16_t targets[MAX_PEERS];
    size_t n = 0;
    portENTER_CRITICAL(&peersMux);
    for (auto &p : peers) {
      if (p.handle == BLE_HS_CONN_HANDLE_NONE) continue;
      if (onlyHandle != BLE_HS_CONN_HANDLE_NONE && p.handle != onlyHandle) continue;
      if (stateChannel ? p.stateSub : p.cmdSub) targets[n++] = p.handle;
    }
    portEXIT_CRITICAL(&peersMux);
    for (size_t i = 0; i < n; i++) {
      bool authen = server->getPeerInfoByHandle(targets[i]).isAuthenticated();
      if (authen) chr->notify(data, len, targets[i]);
      else BLE_LOG("skip notify h%u: not authenticated", targets[i]);
    }
    if (!n) BLE_LOG("notify %s: no subscribers", stateChannel ? "state" : "cmd");
  }

  Peer *findPeer(uint16_t handle) {
    for (auto &p : peers) if (p.handle == handle) return &p;
    return nullptr;
  }

  // For a bonded peer, NimBLE restores encryption and the stored subscriptions
  // (subscribe events with reason RESTORE) before onConnect runs, so whichever
  // callback comes first creates the slot. Call with peersMux held.
  Peer *peerSlot(uint16_t handle) {
    Peer *p = findPeer(handle);
    if (!p && (p = findPeer(BLE_HS_CONN_HANDLE_NONE))) { *p = Peer{}; p->handle = handle; }
    return p;
  }

  // ---- Info and security ------------------------------------------------------

  void updateInfo() {
    uint8_t buf[80];
    size_t n = 0;
    buf[n++] = PROTOCOL_VERSION;
    buf[n++] = FEATURES & 0xFF;
    buf[n++] = FEATURES >> 8;
    buf[n++] = pairingOpen ? INFO_STATUS_PAIRING : 0;
    size_t nameLen = utf8Prefix(serverDescription, 32);
    buf[n++] = nameLen;
    memcpy(buf + n, serverDescription, nameLen); n += nameLen;
    size_t verLen = utf8Prefix(versionString, 32);
    buf[n++] = verLen;
    memcpy(buf + n, versionString, verLen); n += verLen;
    infoChar->setValue(buf, n);
  }

  // Outside the pairing window the device offers neither bonding nor MITM, so
  // a new peer can only get an unauthenticated link, which the secured
  // characteristics refuse. Bonded peers keep their authenticated keys.
  void applySecurity() {
    if (pairingOpen) {
      NimBLEDevice::setSecurityAuth(true, true, true);
      NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
    } else {
      NimBLEDevice::setSecurityAuth(false, false, true);
      NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    }
    NimBLEDevice::setSecurityPasskey(passkey);
  }

  void setPairing(bool open) {
    if (open == pairingOpen && !open) return;
    pairingOpen = open;
    pairingOpenedMs = millis();
    applySecurity();
    updateInfo();
    BLE_LOG("BLE: pairing window %s", open ? "open" : "closed");
  }

  // ---- Commands -----------------------------------------------------------------

  void reply(uint16_t handle, uint8_t op, uint8_t status) {
    uint8_t r[2] = {op, status};
    notifyPeers(cmdChar, r, sizeof(r), false, handle);
  }

  // Loop context: applies a queued command through WLED's JSON API, so the
  // effect matches the web UI exactly.
  void execute(const Command &c) {
    StaticJsonDocument<128> doc;
    JsonObject o = doc.to<JsonObject>();
    uint8_t status = ST_OK;

    switch (c.op) {
      case OP_POWER:
        if (c.arg == 2) o["on"] = "t";
        else if (c.arg <= 1) o["on"] = (bool)c.arg;
        else status = ST_INVALID;
        break;
      case OP_BRIGHTNESS_SET:
        if (c.arg >= 1 && c.arg <= 255) { o["on"] = true; o["bri"] = c.arg; }
        else status = ST_INVALID;
        break;
      case OP_BRIGHTNESS_STEP:
        if (bri == 0) { if (c.arg > 0) o["on"] = true; }
        else o["bri"] = constrain((int)bri + c.arg, 1, 255);
        break;
      case OP_PRESET_APPLY:
        if (presets.has(c.arg)) o["ps"] = c.arg;
        else status = ST_INVALID;
        break;
      case OP_PRESET_NEXT:
      case OP_PRESET_PREV: {
        uint8_t id = presets.step(currentPreset, c.arg == 1, c.op == OP_PRESET_NEXT);
        if (id) o["ps"] = id;
        else status = ST_INVALID;
        break;
      }
      case OP_NIGHTLIGHT:
        if (c.arg == 0) o["nl"]["on"] = false;
        else {
          nightlightActiveOld = false;  // restart the timer with the new duration
          o["nl"]["on"] = true;
          o["nl"]["dur"] = c.arg;
        }
        break;
      case OP_OFF_DISCONNECT:
        o["on"] = false;
        pendingDisconnect = c.connHandle;
        disconnectAtMs = millis() + DISCONNECT_DELAY_MS;
        break;
      default:
        status = ST_UNKNOWN_OP;
    }

    if (status == ST_OK && o.size() > 0) deserializeState(o, CALL_MODE_DIRECT_CHANGE);
    BLE_LOG("BLE: cmd %02x arg %d -> %02x", c.op, c.arg, status);
    reply(c.connHandle, c.op, status);
  }

  // ---- NimBLE callbacks (BLE host task: queue work, don't touch WLED state) ------

  void onConnect(NimBLEServer *, NimBLEConnInfo &info) override {
    portENTER_CRITICAL(&peersMux);
    Peer *p = peerSlot(info.getConnHandle());
    portEXIT_CRITICAL(&peersMux);
    BLE_LOG("conn h%u slot %d", info.getConnHandle(), p ? (int)(p - peers) : -1);
    // Advertising stops on connect; keep it going if there's a free slot.
    if (server->getConnectedCount() < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) NimBLEDevice::startAdvertising();
  }

  void onDisconnect(NimBLEServer *, NimBLEConnInfo &info, int reason) override {
    portENTER_CRITICAL(&peersMux);
    Peer *p = findPeer(info.getConnHandle());
    if (p) *p = Peer{};
    portEXIT_CRITICAL(&peersMux);
    BLE_LOG("disc h%u reason %d", info.getConnHandle(), reason);
  }

  uint32_t onPassKeyDisplay() override { return passkey; }

  void onAuthenticationComplete(NimBLEConnInfo &info) override {
    BLE_LOG("BLE: auth complete enc=%d authen=%d bonded=%d", info.isEncrypted(), info.isAuthenticated(), info.isBonded());
  }

  void onSubscribe(NimBLECharacteristic *chr, NimBLEConnInfo &info, uint16_t subValue) override {
    portENTER_CRITICAL(&peersMux);
    Peer *p = peerSlot(info.getConnHandle());
    if (p) {
      if (chr == stateChar) p->stateSub = subValue & 0x01;
      else if (chr == cmdChar) p->cmdSub = subValue & 0x01;
    }
    portEXIT_CRITICAL(&peersMux);
    BLE_LOG("sub h%u %s=%u enc%d found%d", info.getConnHandle(), chr == stateChar ? "state" : chr == cmdChar ? "cmd" : "?", subValue, info.isEncrypted(), p != nullptr);
  }

  void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &info) override {
    NimBLEAttValue v = chr->getValue();
    const uint8_t *d = v.data();
    size_t len = v.size();

    if (chr == presetChar) {
      static uint8_t pageBuf[PRESET_PAGE_MAX];
      uint8_t start = len >= 1 ? d[0] : 1;
      size_t n = presets.page(start, pageBuf, sizeof(pageBuf));
      presetChar->setValue(pageBuf, n);
      return;
    }
    if (chr != cmdChar || len == 0) return;

    uint8_t op = d[0];
    size_t want;
    switch (op) {
      case OP_POWER: case OP_BRIGHTNESS_SET: case OP_BRIGHTNESS_STEP: case OP_PRESET_APPLY:
      case OP_PRESET_NEXT: case OP_PRESET_PREV: case OP_NIGHTLIGHT:
        want = 2; break;
      case OP_OFF_DISCONNECT:
        want = 1; break;
      default:
        reply(info.getConnHandle(), op, ST_UNKNOWN_OP);
        return;
    }
    if (len != want) { reply(info.getConnHandle(), op, ST_BAD_LENGTH); return; }

    Command c{op, 0, info.getConnHandle()};
    if (want == 2) c.arg = (op == OP_BRIGHTNESS_STEP) ? (int16_t)(int8_t)d[1] : (int16_t)d[1];
    if (xQueueSend(cmdQueue, &c, 0) != pdTRUE) reply(info.getConnHandle(), op, ST_BUSY);
  }

  // ---- Init ---------------------------------------------------------------------

  bool initBle() {
    heapBeforeInit = ESP.getFreeHeap();
    BLE_LOG("BLE: init, heap %u", heapBeforeInit);

    cmdQueue = xQueueCreate(8, sizeof(Command));
    presets.begin();
    if (!cmdQueue || !NimBLEDevice::init(serverDescription)) {
      BLE_LOG("BLE: init failed");
      return false;
    }
    applySecurity();

    server = NimBLEDevice::createServer();
    server->setCallbacks(this, false);
    server->advertiseOnDisconnect(true);  // NimBLE-Arduino 2.x defaults to false

    const uint32_t SECURE_READ  = NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN;
    const uint32_t SECURE_WRITE = NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN;

    NimBLEService *service = server->createService(SERVICE_UUID);
    infoChar   = service->createCharacteristic(CHAR_INFO_UUID, NIMBLE_PROPERTY::READ, 80);
    stateChar  = service->createCharacteristic(CHAR_STATE_UUID, SECURE_READ | NIMBLE_PROPERTY::NOTIFY, sizeof(StatePacket));
    cmdChar    = service->createCharacteristic(CHAR_CMD_UUID, SECURE_WRITE | NIMBLE_PROPERTY::NOTIFY, 16);
    presetChar = service->createCharacteristic(CHAR_PRESET_UUID, SECURE_WRITE | SECURE_READ, PRESET_PAGE_MAX);
    stateChar->setCallbacks(this);
    cmdChar->setCallbacks(this);
    presetChar->setCallbacks(this);

    updateInfo();
    lastValue = readState();
    stateChar->setValue((uint8_t *)&lastValue, sizeof(lastValue));
    uint8_t empty[4] = {0, 0, 0, 0};
    presetChar->setValue(empty, sizeof(empty));

    // 31-byte advert: flags (3) + 128-bit service UUID (18) leaves no room for
    // the name, so it goes in the scan response.
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData advData, scanData;
    advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    advData.addServiceUUID(SERVICE_UUID);
    scanData.setName(serverDescription);
    adv->setAdvertisementData(advData);
    adv->setScanResponseData(scanData);
    bool advOk = adv->start();

    heapAfterInit = ESP.getFreeHeap();
    BLE_LOG("BLE: ready, advertising %d, used %u bytes", advOk, heapBeforeInit - heapAfterInit);
    return true;
  }

  // Advertising can silently fail to restart, e.g. after deleting the bond of
  // a connected peer (ble_gap_unpair() drops the link mid-procedure). Restart
  // it whenever a connection slot is free.
  void keepAdvertising() {
    if (millis() - advCheckMs < 1000) return;
    advCheckMs = millis();
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    if (!adv->isAdvertising() && server->getConnectedCount() < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
      bool ok = adv->start();
      BLE_LOG("adv restart %d", ok);
    }
  }

  // presetsModifiedTime has one-second resolution, so saves later in the same
  // second don't change it: rebuild only once changes have settled for longer
  // than that. Uploads and file-editor changes don't touch it at all, so the
  // file size is checked too.
  void checkPresetsChanged() {
    if (presetsModifiedTime != presetsSeenTime) {
      presetsSeenTime = presetsModifiedTime;
      presetsChangedMs = millis();
      presetsDirty = true;
    }
    if (millis() - presetsSizeCheckMs >= PRESETS_SIZE_CHECK_MS) {
      presetsSizeCheckMs = millis();
      File f = WLED_FS.open(getPresetsFileName(), "r");
      size_t size = f ? f.size() : 0;
      if (f) f.close();
      if (size != presetsSeenSize) {
        presetsSeenSize = size;
        presetsChangedMs = millis();
        presetsDirty = true;
      }
    }
  }

  // The WiFi driver aborts ("Should enable WiFi modem sleep when both WiFi and
  // Bluetooth are enabled") if power save is off while BT runs. WLED defaults
  // to noWifiSleep = true and applies it in initConnection().
  void forceModemSleep() { noWifiSleep = false; }

public:
  void setup() override {
#ifdef BLE_DIAG
    lastResetReason = (int)esp_reset_reason();
    if (rtcLogMagic == RTC_LOG_MAGIC) lastBootLog = String(rtcLog);
    rtcLogMagic = RTC_LOG_MAGIC;
    rtcLog[0] = '\0';
#endif
    if (!enabled) return;
    forceModemSleep();  // before WLED's first initConnection()
#ifndef BLE_DEFER_START
    initDone = initBle();
#endif
  }

  void loop() override {
    if (!initDone) {
      if (enabled && startRequested) {
        startRequested = false;
        WiFi.setSleep(true);
        initDone = initBle();
      }
      return;
    }
    forceModemSleep();  // the WiFi settings page can set it again

    if (pairRequested)   { pairRequested = false; setPairing(true); }
    if (pairingOpen && millis() - pairingOpenedMs > pairWindowSec * 1000UL) setPairing(false);
    if (forgetRequested) { forgetRequested = false; NimBLEDevice::deleteAllBonds(); BLE_LOG("BLE: bonds deleted"); }

    checkPresetsChanged();
    if (presetsDirty && millis() - presetsChangedMs >= PRESETS_SETTLE_MS && presets.rebuild()) presetsDirty = false;

    Command c;
    while (xQueueReceive(cmdQueue, &c, 0) == pdTRUE) execute(c);

    if (pendingDisconnect != BLE_HS_CONN_HANDLE_NONE && (int32_t)(millis() - disconnectAtMs) >= 0) {
      server->disconnect(pendingDisconnect);
      pendingDisconnect = BLE_HS_CONN_HANDLE_NONE;
    }

    updateState();
    keepAdvertising();
  }

  void readFromJsonState(JsonObject &root) override {
    JsonObject ble = root[F("ble")];
    if (ble.isNull()) return;
    if (ble[F("start")] | false) startRequested = true;
    if (ble[F("pair")] | false) pairRequested = true;
    if (ble[F("forget")] | false) forgetRequested = true;
  }

  void addToJsonInfo(JsonObject &root) override {
    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");

#ifdef BLE_DIAG
    JsonArray rr = user.createNestedArray(F("BLE reset reason"));
    rr.add(lastResetReason);
    JsonArray lb = user.createNestedArray(F("BLE last boot"));
    lb.add(lastBootLog);
    JsonArray cl = user.createNestedArray(F("BLE log"));
    cl.add((const char *)rtcLog);
#endif

    JsonArray line = user.createNestedArray(F("BLE"));
    if (!initDone) { line.add(enabled ? F("not running") : F("disabled")); return; }
    uint8_t connected = server->getConnectedCount();
    if (connected) { line.add(connected); line.add(F(" connected")); }
    else line.add(NimBLEDevice::getAdvertising()->isAdvertising() ? F("advertising") : F("idle"));

    JsonArray bonds = user.createNestedArray(F("BLE paired devices"));
    bonds.add(NimBLEDevice::getNumBonds());

    JsonArray pairing = user.createNestedArray(F("BLE pairing"));
    if (pairingOpen) {
      uint32_t left = pairWindowSec - (millis() - pairingOpenedMs) / 1000;
      pairing.add(left);
      pairing.add(F(" s left"));
    } else {
      pairing.add(F("<button class=\"btn btn-xs\" onclick=\"requestJson({ble:{pair:true}});\">Enable</button>"));
    }

    JsonArray heap = user.createNestedArray(F("BLE heap used"));
    heap.add(heapBeforeInit - heapAfterInit);
    heap.add(F(" B"));
  }

  void addToConfig(JsonObject &root) override {
    JsonObject top = root.createNestedObject(FPSTR(_name));
    top[F("enabled")] = enabled;
    top[F("passkey")] = passkey;
    top[F("pairingWindowSec")] = pairWindowSec;
    top[F("forgetAllDevices")] = false;
  }

  bool readFromConfig(JsonObject &root) override {
    JsonObject top = root[FPSTR(_name)];
    bool complete = !top.isNull();
    complete &= getJsonValue(top[F("enabled")], enabled, true);
    complete &= getJsonValue(top[F("passkey")], passkey, 0);
    complete &= getJsonValue(top[F("pairingWindowSec")], pairWindowSec, 120);
    bool forget = false;
    getJsonValue(top[F("forgetAllDevices")], forget, false);

    if (passkey < 100000 || passkey > 999999) {
      passkey = 100000 + esp_random() % 900000;
      configNeedsWrite = true;  // persist the generated passkey
    }
    pairWindowSec = constrain(pairWindowSec, 10, 3600);
    if (forget) { forgetRequested = true; configNeedsWrite = true; }  // one-shot; saved back as false
    if (initDone) applySecurity();
    return complete;
  }

  void appendConfigData(Print &uiScript) override {
    uiScript.print(F("addInfo('BLEControl:passkey',1,'6 digits, entered on the watch when pairing');"));
    uiScript.print(F("addInfo('BLEControl:pairingWindowSec',1,'s. Open the window from the Info page');"));
    uiScript.print(F("addInfo('BLEControl:forgetAllDevices',1,'Unpairs every watch on save');"));
    uiScript.print(F("addInfo('BLEControl:enabled',1,'Reboot to apply');"));
  }
};

const char BleControlUsermod::_name[] PROGMEM = "BLEControl";

} // namespace WLEDBle

static WLEDBle::BleControlUsermod bleControlUsermod;
REGISTER_USERMOD(bleControlUsermod);
