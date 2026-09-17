/*
  xdrv_124_tinyc_usb.h — an FTDI USB-serial adapter as a TinyC primitive.

  Gives a TinyC script a SERIAL link to any device that hangs on the ESP32's USB
  host port behind an FTDI chip. The protocol on top lives in the script, exactly
  like the SPP family: the same primitive serves a VarioLab, a GPS mouse, an
  inverter or a laboratory instrument, and it changes without reflashing.

  The reason it exists: gemu's VarioLab has its FTDI soldered in and no way to
  reach the UART before it, so the only way in is to BE a USB host (12.09.2026).

  ---------------------------------------------------------------------------------------
  ESP32-S3 / S2 ONLY, AND ONLY WITH TWO USB PORTS.
  ---------------------------------------------------------------------------------------
  USB host needs the OTG peripheral, which the original ESP32 does not have at all
  and the C3/C6 have only as a device. On the S3 the host uses GPIO19/20 — the same
  lines as the native USB. A board with ONE socket must therefore give up the USB
  console (ARDUINO_USB_CDC_ON_BOOT) to be a host. gemu's devkit has USB-COM *and*
  USB-OTG, so the console stays where it is and the OTG socket is free.

  ⚠️ The host must supply VBUS. If nothing enumerates although the cable is right,
  that is the first thing to measure — many OTG sockets have no 5 V switch.

  ⭐ NOTHING HAD TO BE ADDED TO THE FRAMEWORK. Tasmota's own arduino-esp32 branch
  (3.3.8 on ESP-IDF 5.5) already ships libusb.a for the S3 with usb_host_install,
  usb_host_client_register, usb_host_lib_handle_events and usb_host_transfer_alloc
  exported, plus usb/usb_host.h and the CONFIG_USB_HOST_* settings. Verified by
  linking and running on 12.09.2026: the VarioLab enumerated as 0403:6001 (FT232R).

  ---------------------------------------------------------------------------------------
  WHY NO CDC-ACM COMPONENT.
  ---------------------------------------------------------------------------------------
  An FTDI is NOT a CDC device — it is vendor-specific, so no class driver adopts it
  anyway. Espressif's usb_host_cdc_acm + usb_host_ftdi_vcp would do it, but that is
  over 70 KB of source with its own component layout that does not fit PlatformIO's
  library folder. What an FT232 actually needs is four control requests and one bulk
  pair, and that is what is below.

  ⚠️ THE ONE FTDI ODDITY: every IN packet starts with TWO STATUS BYTES (modem and
  line status). They are stripped in the completion callback. Forget that and the
  data has two bytes of rubbish every 62 bytes — which looks like a baud rate error
  and sends you hunting in the wrong place.

  ---------------------------------------------------------------------------------------
  READS DO NOT BLOCK — same rule as SPP.
  ---------------------------------------------------------------------------------------
  usbRead() returns what has arrived and comes back at once. The script waits by
  itself and keeps control; firmware that waited would hang the VM on the peer's
  timeouts.
*/

#ifndef _XDRV_124_TINYC_USB_H_
#define _XDRV_124_TINYC_USB_H_

#if defined(USE_TINYC_USBSERIAL) && (defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2))

#include "usb/usb_host.h"
#include "freertos/semphr.h"

// States as usbState() reports them — deliberately the same ladder as SPP.
#define TC_USB_AUS      0   // stack not installed
#define TC_USB_BEREIT   1   // host running, no device
#define TC_USB_GERAET   2   // FTDI detected, not opened yet
#define TC_USB_OFFEN    3   // open, data flowing
#define TC_USB_FEHLER   4   // opening failed

// ⚠️ BIG ENOUGH TO OUTLAST A WI-FI HICCUP. At 230400 baud 23 kB/s come in;
// 2048 bytes were 89 ms of headroom, and that is exactly how long the script
// may then stall. Measured 2026-09-12: at 17,7 kB/s from a real VarioLab
// 3 bytes were lost — in this protocol that means a SHIFTED frame, not one
// missing value. 16 kB are 0,7 s of headroom and cost nothing by comparison:
// the S3 has 8 MB of PSRAM free.
#define TC_USB_RING     16384   // receive ring
#define TC_USB_MPS      64      // full speed, bulk
#define TC_USB_IN_N     4       // IN transfers in flight at once

// FTDI-Steuerbefehle (herstellereigen, bmRequestType 0x40)
#define FTDI_RESET          0x00
#define FTDI_MODEM_CTRL     0x01
#define FTDI_SET_FLOW       0x02
#define FTDI_SET_BAUD       0x03
#define FTDI_SET_DATA       0x04

struct TcUsbLage {
  usb_host_client_handle_t klient = nullptr;
  usb_device_handle_t      geraet = nullptr;
  TaskHandle_t             aufgabe = nullptr;
  SemaphoreHandle_t        ctrl_fertig = nullptr;
  SemaphoreHandle_t        out_fertig = nullptr;
  usb_transfer_t          *ctrl = nullptr;
  usb_transfer_t          *out = nullptr;
  usb_transfer_t          *in[TC_USB_IN_N] = { nullptr };
  uint8_t   ep_in = 0, ep_out = 0, schnittstelle = 0;
  volatile uint8_t  state = TC_USB_AUS;
  volatile bool     weiter = false;
  volatile bool     angesteckt = false;   // device present, still has to be opened
  uint8_t   adresse = 0;
  uint16_t  vid = 0, pid = 0;
  uint32_t  baud = 0;
  volatile uint32_t rx = 0, tx = 0, verloren = 0;
  // Ring
  volatile uint16_t kopf = 0, fuss = 0;
  uint8_t   ring[TC_USB_RING];
  portMUX_TYPE sperre = portMUX_INITIALIZER_UNLOCKED;
} TcUsb;

/*───────────────────────── Ring buffer ────────────────────────────────────*/
static inline uint16_t TcUsbAvailable(void) {
  uint16_t k = TcUsb.kopf, f = TcUsb.fuss;
  return (k >= f) ? (k - f) : (uint16_t)(TC_USB_RING - f + k);
}
static void TcUsbPush(const uint8_t *d, uint16_t len) {
  taskENTER_CRITICAL(&TcUsb.sperre);
  for (uint16_t i = 0; i < len; i++) {
    uint16_t neu = (TcUsb.kopf + 1) % TC_USB_RING;
    if (neu == TcUsb.fuss) { TcUsb.verloren++; break; }   // full — keep the older
    TcUsb.ring[TcUsb.kopf] = d[i];
    TcUsb.kopf = neu;
  }
  taskEXIT_CRITICAL(&TcUsb.sperre);
}
static uint16_t TcUsbPull(uint8_t *ziel, uint16_t max) {
  uint16_t n = 0;
  taskENTER_CRITICAL(&TcUsb.sperre);
  while (n < max && TcUsb.fuss != TcUsb.kopf) {
    ziel[n++] = TcUsb.ring[TcUsb.fuss];
    TcUsb.fuss = (TcUsb.fuss + 1) % TC_USB_RING;
  }
  taskEXIT_CRITICAL(&TcUsb.sperre);
  return n;
}

/*───────────────────────── Transfers ──────────────────────────────────────*/
// ⚠️ Every callback runs in the USB task. Do NOT compute and do NOT log in
// here — only store and hand on.
struct TcUsbTeile {

  static void in_fertig(usb_transfer_t *t) {
    if (USB_TRANSFER_STATUS_COMPLETED == t->status && t->actual_num_bytes > 2) {
      // ⚠️ THE FIRST TWO BYTES ARE MODEM AND LINE STATUS, NOT DATA.
      TcUsbPush(t->data_buffer + 2, (uint16_t)(t->actual_num_bytes - 2));
      TcUsb.rx += (t->actual_num_bytes - 2);
    }
    if (TcUsb.weiter && TcUsb.geraet) { usb_host_transfer_submit(t); }
  }

  static void out_fertig(usb_transfer_t *t) {
    if (TcUsb.out_fertig) { xSemaphoreGive(TcUsb.out_fertig); }
  }

  static void ctrl_fertig(usb_transfer_t *t) {
    if (TcUsb.ctrl_fertig) { xSemaphoreGive(TcUsb.ctrl_fertig); }
  }

  // Look at the device on this address and keep it if it is an FTDI.
  // Split out of `ereignis` so that it can also be searched for without an
  // event — see TcUsbSuchen().
  static bool annehmen(uint8_t adresse) {
    usb_device_handle_t g;
    if (usb_host_device_open(TcUsb.klient, adresse, &g) != ESP_OK) { return false; }
    const usb_device_desc_t *d = nullptr;
    if (usb_host_get_device_descriptor(g, &d) == ESP_OK && d) {
      if (0x0403 == d->idVendor) {                // FTDI
        TcUsb.geraet = g;
        TcUsb.adresse = adresse;
        TcUsb.vid = d->idVendor;
        TcUsb.pid = d->idProduct;
        TcUsb.angesteckt = true;                  // usbOpen() does the opening
        if (TC_USB_OFFEN != TcUsb.state) { TcUsb.state = TC_USB_GERAET; }
        return true;                              // offen lassen!
      }
    }
    usb_host_device_close(TcUsb.klient, g);       // not ours
    return false;
  }

  static void ereignis(const usb_host_client_event_msg_t *msg, void *arg) {
    if (USB_HOST_CLIENT_EVENT_NEW_DEV == msg->event) {
      annehmen(msg->new_dev.address);
    }
    else if (USB_HOST_CLIENT_EVENT_DEV_GONE == msg->event) {
      TcUsb.angesteckt = false;
      TcUsb.state = TC_USB_BEREIT;
      TcUsb.geraet = nullptr;                     // usbClose() does the cleanup
    }
  }

  static void aufgabe(void *arg) {
    while (TcUsb.weiter) {
      uint32_t flags;
      // ⚠️⚠️ DO NOT WAIT HERE — this was the brake on the whole bridge.
      //
      // Library events are plugging and unplugging, so they are seconds
      // apart. Blocking 50 ms here holds up the TRANSFERS, because those are
      // dispatched in the SAME task: per round only what happens to be
      // finished gets through.
      //
      // Measured 2026-09-12 against a VarioLab at 230400 baud:
      //   receiving  4 IN transfers of 62 B per round = 4,96 kB/s
      //   sending    1 OUT transfer of 64 B per round = 1,2 kB/s
      // Both are arithmetic of the 50 ms round, not of the wire — that one
      // carries 23 kB/s. The numbers matched the measurement to the byte,
      // and that is exactly how the fault was spotted.
      //
      // The waiting now happens where the events arrive: in the client
      // handler, which returns AS SOON AS there is one.
      usb_host_lib_handle_events(0, &flags);
      if (TcUsb.klient) { usb_host_client_handle_events(TcUsb.klient, pdMS_TO_TICKS(50)); }
      else              { vTaskDelay(pdMS_TO_TICKS(5)); }   // no client: do not spin
    }
    TcUsb.aufgabe = nullptr;
    vTaskDelete(nullptr);
  }
};

/*───────────────── Find a device that is already attached ─────────────────*/
// ⚠️⚠️ NEW_DEV ONLY ARRIVES ON PLUG-IN. Starting the host while the device
// has long been attached — or missing the event because it came before the
// client was registered — means waiting for something that will not happen
// again. The cable is seated, the display says "no device", and the fault is
// looked for in the device instead of the driver (2026-09-12, exactly that:
// after a restart of the script the attached VarioLab stayed invisible).
//
// So after the start we look ONCE at what is already there. This is not a
// poll: it runs right after usbInit() and never again — unplugging and
// re-plugging is what the events are for.
static void TcUsbSuchen(void) {
  if (!TcUsb.klient || TC_USB_AUS == TcUsb.state) { return; }
  uint8_t adressen[8];
  int anzahl = 0;
  if (usb_host_device_addr_list_fill(sizeof(adressen), adressen, &anzahl) != ESP_OK) {
    return;
  }
  for (int i = 0; i < anzahl; i++) {
    if (TcUsbTeile::annehmen(adressen[i])) {
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: usbInit — %04X:%04X hing schon an der Buchse"),
             TcUsb.vid, TcUsb.pid);
      return;
    }
  }
}

/*───────────────────────── Steuerbefehle an den FTDI ──────────────────────*/
static bool TcUsbCtrl(uint8_t befehl, uint16_t wert, uint16_t index) {
  if (!TcUsb.geraet || !TcUsb.ctrl) { return false; }
  usb_setup_packet_t *s = (usb_setup_packet_t *)TcUsb.ctrl->data_buffer;
  s->bmRequestType = 0x40;                  // out, vendor specific, device
  s->bRequest = befehl;
  s->wValue = wert;
  s->wIndex = index;
  s->wLength = 0;
  TcUsb.ctrl->num_bytes = sizeof(usb_setup_packet_t);
  TcUsb.ctrl->device_handle = TcUsb.geraet;
  TcUsb.ctrl->bEndpointAddress = 0;
  TcUsb.ctrl->callback = TcUsbTeile::ctrl_fertig;
  TcUsb.ctrl->context = nullptr;
  if (usb_host_transfer_submit_control(TcUsb.klient, TcUsb.ctrl) != ESP_OK) { return false; }
  if (xSemaphoreTake(TcUsb.ctrl_fertig, pdMS_TO_TICKS(500)) != pdTRUE) { return false; }
  return (USB_TRANSFER_STATUS_COMPLETED == TcUsb.ctrl->status);
}

/*  The FT232 divisor against its 3 MHz base clock, with the three
    ⚠️ Die Zuordnung der Achtel ist NICHT fortlaufend — sie steht so im
    Datenblatt und im Linux-Treiber. 230400 ergibt Teiler 13 und damit
    230769 Baud, also 0,16 % daneben; das tun alle Treiber so.              */
static uint32_t TcUsbTeiler(uint32_t baud) {
  static const uint8_t bruch[8] = { 0, 3, 2, 4, 1, 5, 6, 7 };
  if (!baud) { return 0; }
  uint32_t mal8 = (3000000UL * 8) / baud;
  uint32_t ganz = mal8 >> 3;
  uint32_t rest = mal8 & 7;
  uint32_t code = ganz | ((uint32_t)bruch[rest] << 14);
  if (1 == code)      { code = 0; }         // 2 MBaud
  else if (0x4001 == code) { code = 1; }    // 3 MBaud
  return code;
}

static bool TcUsbBaud(uint32_t baud) {
  uint32_t code = TcUsbTeiler(baud);
  if (!TcUsbCtrl(FTDI_SET_BAUD, (uint16_t)(code & 0xFFFF), (uint16_t)(code >> 16))) { return false; }
  TcUsb.baud = baud;
  return true;
}

/*───────────────────────── Anmelden und Abbauen ───────────────────────────*/
static bool TcUsbInit(void) {
  if (TcUsb.state != TC_USB_AUS) {
    // ⚠️ LOOK HERE TOO. The stack is already running — but perhaps without a
    // device, because the plug-in happened before anyone was listening.
    // Without this line a restart of the script would have no effect, and the
    // only way back would be pulling the plug. That, however, is the first
    // thing one reaches for when the display says "no device" — and then it
    // can no longer be told apart whether it was the plug or the missed
    // event (2026-09-12: for gemu it WAS the plug, which did not go deep
    // enough into the socket because of the case).
    if (TC_USB_BEREIT == TcUsb.state) { TcUsbSuchen(); }
    return true;
  }
  const usb_host_config_t hk = { .skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1 };
  esp_err_t e = usb_host_install(&hk);
  if (e != ESP_OK) {
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: usbInit — usb_host_install %s"), esp_err_to_name(e));
    return false;
  }
  const usb_host_client_config_t kk = {
    .is_synchronous = false,
    .max_num_event_msg = 5,
    .async = { .client_event_callback = TcUsbTeile::ereignis, .callback_arg = nullptr },
  };
  if (usb_host_client_register(&kk, &TcUsb.klient) != ESP_OK) {
    usb_host_uninstall();
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: usbInit — client_register fehlgeschlagen"));
    return false;
  }
  TcUsb.ctrl_fertig = xSemaphoreCreateBinary();
  TcUsb.out_fertig  = xSemaphoreCreateBinary();
  TcUsb.weiter = true;
  if (xTaskCreatePinnedToCore(TcUsbTeile::aufgabe, "tc_usb", 4096, nullptr, 5, &TcUsb.aufgabe, 0) != pdPASS) {
    TcUsb.weiter = false;
    usb_host_client_deregister(TcUsb.klient); TcUsb.klient = nullptr;
    usb_host_uninstall();
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: usbInit — Aufgabe liess sich nicht anlegen"));
    return false;
  }
  TcUsb.state = TC_USB_BEREIT;
  AddLog(LOG_LEVEL_INFO, PSTR("TCC: usbInit — Host laeuft, warte auf FTDI"));
  // Give the event task a moment, then look at what is already attached.
  vTaskDelay(pdMS_TO_TICKS(150));
  TcUsbSuchen();
  return true;
}

static void TcUsbClose(void) {
  TcUsb.weiter = false;                  // stops the IN transfers being resubmitted
  delay(60);
  for (int i = 0; i < TC_USB_IN_N; i++) {
    if (TcUsb.in[i]) { usb_host_transfer_free(TcUsb.in[i]); TcUsb.in[i] = nullptr; }
  }
  if (TcUsb.out)  { usb_host_transfer_free(TcUsb.out);  TcUsb.out = nullptr; }
  if (TcUsb.ctrl) { usb_host_transfer_free(TcUsb.ctrl); TcUsb.ctrl = nullptr; }
  if (TcUsb.geraet) {
    usb_host_interface_release(TcUsb.klient, TcUsb.geraet, TcUsb.schnittstelle);
    usb_host_device_close(TcUsb.klient, TcUsb.geraet);
    TcUsb.geraet = nullptr;
  }
  TcUsb.weiter = true;                   // the task keeps running
  TcUsb.kopf = TcUsb.fuss = 0;
  if (TcUsb.state != TC_USB_AUS) { TcUsb.state = TC_USB_BEREIT; }
}

static void TcUsbDeinit(void) {
  if (TC_USB_AUS == TcUsb.state) { return; }
  TcUsbClose();
  TcUsb.weiter = false;
  delay(150);                            // give the task time to leave
  if (TcUsb.klient) { usb_host_client_deregister(TcUsb.klient); TcUsb.klient = nullptr; }
  usb_host_uninstall();
  if (TcUsb.ctrl_fertig) { vSemaphoreDelete(TcUsb.ctrl_fertig); TcUsb.ctrl_fertig = nullptr; }
  if (TcUsb.out_fertig)  { vSemaphoreDelete(TcUsb.out_fertig);  TcUsb.out_fertig = nullptr; }
  TcUsb.state = TC_USB_AUS;
  AddLog(LOG_LEVEL_INFO, PSTR("TCC: usbDeinit — Stapel abgebaut"));
}

/*───────────────────────── Opening ────────────────────────────────────────*/
/*  Finds the first interface in the active configuration that has a bulk
    pair, claims it and sets 8N1 without flow control.                     */
static bool TcUsbOpen(uint32_t baud) {
  if (!TcUsbInit()) { return false; }
  if (TC_USB_OFFEN == TcUsb.state) { return TcUsbBaud(baud); }
  if (!TcUsb.geraet) { return false; }                  // nothing attached yet

  const usb_config_desc_t *cfg = nullptr;
  if (usb_host_get_active_config_descriptor(TcUsb.geraet, &cfg) != ESP_OK || !cfg) { return false; }

  int off = 0;
  TcUsb.ep_in = TcUsb.ep_out = 0;
  const usb_intf_desc_t *intf = usb_parse_interface_descriptor(cfg, 0, 0, &off);
  if (!intf) { return false; }
  TcUsb.schnittstelle = intf->bInterfaceNumber;
  for (int i = 0; i < intf->bNumEndpoints; i++) {
    int eoff = off;
    const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(intf, i, cfg->wTotalLength, &eoff);
    if (!ep) { continue; }
    if (USB_BM_ATTRIBUTES_XFER_BULK != (ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK)) { continue; }
    if (ep->bEndpointAddress & 0x80) { TcUsb.ep_in = ep->bEndpointAddress; }
    else                             { TcUsb.ep_out = ep->bEndpointAddress; }
  }
  if (!TcUsb.ep_in || !TcUsb.ep_out) {
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: usbOpen — kein Bulk-Paar gefunden"));
    TcUsb.state = TC_USB_FEHLER;
    return false;
  }
  if (usb_host_interface_claim(TcUsb.klient, TcUsb.geraet, TcUsb.schnittstelle, 0) != ESP_OK) {
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: usbOpen — Schnittstelle nicht zu belegen"));
    TcUsb.state = TC_USB_FEHLER;
    return false;
  }

  usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 8, 0, &TcUsb.ctrl);
  usb_host_transfer_alloc(TC_USB_MPS, 0, &TcUsb.out);
  if (!TcUsb.ctrl || !TcUsb.out) { TcUsbClose(); return false; }

  // Reset, 8 data bits, no parity, one stop bit, no flow control.
  TcUsbCtrl(FTDI_RESET, 0, 0);
  if (!TcUsbBaud(baud)) {
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: usbOpen — Baudrate wurde nicht angenommen"));
    TcUsbClose(); TcUsb.state = TC_USB_FEHLER; return false;
  }
  TcUsbCtrl(FTDI_SET_DATA, 0x0008, 0);
  TcUsbCtrl(FTDI_SET_FLOW, 0, 0);
  TcUsbCtrl(FTDI_MODEM_CTRL, 0x0303, 0);      // DTR and RTS on

  for (int i = 0; i < TC_USB_IN_N; i++) {
    if (usb_host_transfer_alloc(TC_USB_MPS, 0, &TcUsb.in[i]) != ESP_OK) { break; }
    TcUsb.in[i]->device_handle = TcUsb.geraet;
    TcUsb.in[i]->bEndpointAddress = TcUsb.ep_in;
    TcUsb.in[i]->callback = TcUsbTeile::in_fertig;
    TcUsb.in[i]->context = nullptr;
    TcUsb.in[i]->num_bytes = TC_USB_MPS;
    usb_host_transfer_submit(TcUsb.in[i]);
  }
  TcUsb.state = TC_USB_OFFEN;
  AddLog(LOG_LEVEL_INFO, PSTR("TCC: usbOpen — %04X:%04X offen, %u Baud, EP %02X/%02X"),
         TcUsb.vid, TcUsb.pid, (unsigned)baud, TcUsb.ep_in, TcUsb.ep_out);
  return true;
}

/*───────────────────────── Senden ─────────────────────────────────────────*/
static int32_t TcUsbWrite(const uint8_t *d, uint16_t len) {
  if (TC_USB_OFFEN != TcUsb.state || !TcUsb.out) { return -1; }
  uint16_t gesamt = 0;
  while (gesamt < len) {
    uint16_t teil = len - gesamt;
    if (teil > TC_USB_MPS) { teil = TC_USB_MPS; }
    memcpy(TcUsb.out->data_buffer, d + gesamt, teil);
    TcUsb.out->device_handle = TcUsb.geraet;
    TcUsb.out->bEndpointAddress = TcUsb.ep_out;
    TcUsb.out->callback = TcUsbTeile::out_fertig;
    TcUsb.out->context = nullptr;
    TcUsb.out->num_bytes = teil;
    if (usb_host_transfer_submit(TcUsb.out) != ESP_OK) { return gesamt ? gesamt : -1; }
    if (xSemaphoreTake(TcUsb.out_fertig, pdMS_TO_TICKS(300)) != pdTRUE) { return gesamt ? gesamt : -1; }
    if (USB_TRANSFER_STATUS_COMPLETED != TcUsb.out->status) { return gesamt ? gesamt : -1; }
    gesamt += teil;
    TcUsb.tx += teil;
  }
  return gesamt;
}

#endif  // USE_TINYC_USBSERIAL && S3/S2
#endif  // _XDRV_124_TINYC_USB_H_
