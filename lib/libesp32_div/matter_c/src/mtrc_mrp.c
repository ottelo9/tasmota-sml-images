// mtrc_mrp.c — Matter Message Reliability Protocol. See mtrc_mrp.h. GPLv3.

#include "mtrc_mrp.h"
#include "mtrc_frame.h"
#include <string.h>

// MRP backoff (Core Spec §4.12.2.1), integer form (no float):
//   interval(n) = base * MARGIN * BASE^max(0, n - THRESHOLD)
// with BASE = 1.6 = 8/5, MARGIN = 1.1 = 11/10, THRESHOLD = 1.
// Production adds 0..MRP_BACKOFF_JITTER (0.25) via the port RNG; omitted
// here so retransmit timing is deterministic for the self-test.
#define MRP_BACKOFF_THRESHOLD 1
static uint32_t mrp_interval(uint32_t base, int attempt /*1-based tx count*/) {
  // attempt 1 (first send) -> wait before retransmit #1 uses exponent 0.
  int e = attempt - 1 - MRP_BACKOFF_THRESHOLD;
  if (e < 0) e = 0;
  uint64_t t = (uint64_t)base * 11u / 10u;   // * MARGIN
  for (int i = 0; i < e; i++) t = t * 8u / 5u; // * 1.6 each step
  return (uint32_t)t;
}

void mtrc_mrp_init(mtrc_mrp_ctx *c, uint32_t base_ms) {
  memset(c, 0, sizeof(*c));
  c->base_ms = base_ms ? base_ms : MTRC_MRP_DEFAULT_BASE_MS;
}

int mtrc_mrp_on_send(mtrc_mrp_ctx *c, const uint8_t *frame, size_t len,
                     uint32_t msg_counter, bool reliable, uint32_t now_ms) {
  if (!reliable) return 1;                 // nothing to track
  if (len > MTRC_MRP_MAX_FRAME) return 0;
  memcpy(c->tx_frame, frame, len);
  c->tx_len = len;
  c->tx_msg_counter = msg_counter;
  c->tx_attempts = 1;                      // this was transmission #1
  c->tx_pending = true;
  c->tx_next_ms = now_ms + mrp_interval(c->base_ms, c->tx_attempts);
  return 1;
}

int mtrc_mrp_on_ack(mtrc_mrp_ctx *c, uint32_t ack_counter) {
  if (c->tx_pending && ack_counter == c->tx_msg_counter) {
    c->tx_pending = false;
    return 1;
  }
  return 0;
}

int mtrc_mrp_is_duplicate(mtrc_mrp_ctx *c, uint32_t msg_counter) {
  if (!c->have_rx) {
    c->have_rx = true; c->rx_max = msg_counter; c->rx_window = 0;
    return 0;
  }
  if (msg_counter == c->rx_max) return 1;
  if (msg_counter > c->rx_max) {
    uint32_t shift = msg_counter - c->rx_max;
    // mark old rx_max in the window, then shift the window up
    if (shift >= 32) c->rx_window = 0;
    else c->rx_window = (c->rx_window << shift) | (1u << (shift - 1));
    c->rx_max = msg_counter;
    return 0;
  }
  // older than rx_max
  uint32_t back = c->rx_max - msg_counter;     // 1..
  if (back > 32) return 1;                      // too old -> treat as dup/drop
  uint32_t bit = 1u << (back - 1);
  if (c->rx_window & bit) return 1;             // already seen
  c->rx_window |= bit;
  return 0;
}

mtrc_mrp_tick_result mtrc_mrp_tick(mtrc_mrp_ctx *c, uint32_t now_ms,
                                   uint8_t *out, size_t cap, size_t *out_len) {
  if (!c->tx_pending) return MTRC_MRP_IDLE;
  if ((int32_t)(now_ms - c->tx_next_ms) < 0) return MTRC_MRP_IDLE;  // not due
  if (c->tx_attempts >= MTRC_MRP_MAX_TRANSMISSIONS) {
    c->tx_pending = false;
    return MTRC_MRP_TIMEOUT;
  }
  if (c->tx_len > cap) { c->tx_pending = false; return MTRC_MRP_TIMEOUT; }
  memcpy(out, c->tx_frame, c->tx_len);
  if (out_len) *out_len = c->tx_len;
  c->tx_attempts++;
  c->tx_next_ms = now_ms + mrp_interval(c->base_ms, c->tx_attempts);
  return MTRC_MRP_RETRANSMIT;
}

int mtrc_mrp_build_ack(uint8_t *out, size_t cap,
                       uint16_t session_id, uint32_t local_counter,
                       uint16_t exchange_id, bool initiator,
                       uint32_t ack_counter) {
  mtrc_msg_header mh; memset(&mh, 0, sizeof(mh));
  mh.session_id  = session_id;
  mh.session_type = 0;                 // unicast
  mh.msg_counter = local_counter;
  mh.dsiz = MTRC_DSIZ_NONE;

  mtrc_proto_header ph; memset(&ph, 0, sizeof(ph));
  ph.initiator   = initiator;          // reflects who owns the exchange
  ph.ack         = true;               // A flag + ack counter
  ph.reliability = false;              // a standalone ack is not itself reliable
  ph.opcode      = MTRC_SC_STANDALONE_ACK;
  ph.exchange_id = exchange_id;
  ph.protocol_id = MTRC_PROTO_SECURE_CHANNEL;
  ph.ack_counter = ack_counter;

  return mtrc_frame_encode(out, cap, &mh, &ph, NULL, 0);
}
