#include "ble_link.h"

#if defined(ARDUINO_ARCH_ESP32)  // the host preview build has no radio

// NimBLE's own API rather than the core's BLE library: that C++ wrapper costs more flash
// than everything this file needs. See the design doc for the numbers.
#include <Arduino.h>
#include <esp_mac.h>
#include <freertos/stream_buffer.h>
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define BLE_RX_BYTES 4096  // same as the serial receive buffer: a burst of hook lines fits

// The UUIDs of ble_link.h, byte-reversed: NimBLE stores 128-bit UUIDs little-endian.
static const ble_uuid128_t SERVICE_UUID = BLE_UUID128_INIT(
    0x84, 0x0f, 0xeb, 0x84, 0x6f, 0xc2, 0x48, 0x91, 0x5a, 0x4f, 0x73, 0x69, 0x6d, 0xaa, 0x97, 0xee);
static const ble_uuid128_t RX_UUID = BLE_UUID128_INIT(
    0xbc, 0xce, 0x05, 0xaa, 0xfc, 0x31, 0xac, 0xb4, 0xe7, 0x4a, 0xf8, 0x1b, 0xa9, 0x93, 0x1d, 0xf9);

// Writes arrive on the NimBLE host task; loop() reads. One writer and one reader is the case a
// stream buffer is safe for without a lock.
static StreamBufferHandle_t rx = nullptr;
static volatile bool connected = false;
static uint8_t ownAddrType;
static char name[20];

// Without this the core frees the controller's memory at boot, before bleBegin() runs.
extern "C" bool btInUse() { return true; }

static int onRx(uint16_t, uint16_t, struct ble_gatt_access_ctxt* ctxt, void*) {
  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
  // Full buffer: drop the rest. The line reassembler then sees one garbled line and warns,
  // like serial overflow; it never blocks the radio task.
  for (const struct os_mbuf* om = ctxt->om; om; om = SLIST_NEXT(om, om_next)) {
    xStreamBufferSend(rx, om->om_data, om->om_len, 0);
  }
  return 0;
}

static const struct ble_gatt_chr_def CHARACTERISTICS[] = {
    {.uuid = &RX_UUID.u, .access_cb = onRx, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
    {},
};
static const struct ble_gatt_svc_def SERVICES[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &SERVICE_UUID.u, .characteristics = CHARACTERISTICS},
    {},
};

static void advertise();

static int onGap(struct ble_gap_event* e, void*) {
  switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
      connected = e->connect.status == 0;
      if (!connected) advertise();
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      connected = false;
      advertise();  // the next host (or the same one, back) can find us
      break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
      advertise();
      break;
  }
  return 0;
}

static void advertise() {
  struct ble_hs_adv_fields adv = {};
  adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  adv.uuids128 = &SERVICE_UUID;  // hosts scan by service, not by name
  adv.num_uuids128 = 1;
  adv.uuids128_is_complete = 1;
  ble_gap_adv_set_fields(&adv);
  struct ble_hs_adv_fields rsp = {};  // the name goes in the scan response: the UUID fills the packet
  rsp.name = (const uint8_t*)name;
  rsp.name_len = strlen(name);
  rsp.name_is_complete = 1;
  ble_gap_adv_rsp_set_fields(&rsp);
  struct ble_gap_adv_params params = {};
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  ble_gap_adv_start(ownAddrType, nullptr, BLE_HS_FOREVER, &params, onGap, nullptr);
}

static void onSync() {
  ble_hs_id_infer_auto(0, &ownAddrType);
  advertise();
}

static void hostTask(void*) {
  nimble_port_run();  // returns only on nimble_port_stop()
  nimble_port_freertos_deinit();
}

void bleBegin() {
  rx = xStreamBufferCreate(BLE_RX_BYTES, 1);
  if (!rx || nimble_port_init() != ESP_OK) return;
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(name, sizeof(name), "clawd-mood-%02X%02X", mac[4], mac[5]);  // tell two boards apart
  ble_hs_cfg.sync_cb = onSync;
  ble_svc_gap_init();
  ble_svc_gatt_init();
  ble_gatts_count_cfg(SERVICES);
  ble_gatts_add_svcs(SERVICES);
  ble_svc_gap_device_name_set(name);
  nimble_port_freertos_init(hostTask);
}

size_t bleRead(char* buf, size_t cap) {
  return rx ? xStreamBufferReceive(rx, buf, cap, 0) : 0;
}

bool bleConnected() { return connected; }

#else

void bleBegin() {}
size_t bleRead(char*, size_t) { return 0; }
bool bleConnected() { return false; }

#endif
