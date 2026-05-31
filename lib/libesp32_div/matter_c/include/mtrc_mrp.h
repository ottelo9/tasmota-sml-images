// mtrc_mrp.h — Matter Message Reliability Protocol for matter_c.
//
// Matter runs over UDP; MRP adds at-least-once reliable delivery (Core
// Spec §4.12). A message with the R (reliability) flag must be
// acknowledged by the peer (A flag + Ack Counter == the message's
// counter), either piggybacked on a response or as a Secure Channel
// Standalone Acknowledgement (opcode 0x10). The sender retransmits with
// exponential backoff until acked or MRP_MAX_TRANSMISSIONS is reached.
// Receivers do replay/duplicate detection on the message counter.
//
// This module tracks one outstanding reliable message per context
// (one exchange), provides backoff retransmit timing, builds standalone
// acks, and does sliding-window duplicate detection.
//
// GPLv3. Implemented from the Matter spec, not converted from Berry.

#ifndef MTRC_MRP_H
#define MTRC_MRP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MTRC_MRP_MAX_FRAME
#define MTRC_MRP_MAX_FRAME       1280   // IPv6 UDP MTU; holds a retransmit copy
#endif
#define MTRC_MRP_MAX_TRANSMISSIONS 5    // 1 original + 4 retransmissions
#define MTRC_MRP_DEFAULT_BASE_MS  300   // active retransmit base (idle = 500)

// tick() outcomes
typedef enum {
  MTRC_MRP_IDLE        = 0,  // nothing to do
  MTRC_MRP_RETRANSMIT  = 1,  // out[] holds a frame to resend now
  MTRC_MRP_TIMEOUT     = 2,  // gave up after MAX_TRANSMISSIONS (pending cleared)
} mtrc_mrp_tick_result;

typedef struct {
  // outstanding reliable transmission
  bool     tx_pending;
  uint8_t  tx_frame[MTRC_MRP_MAX_FRAME];
  size_t   tx_len;
  uint32_t tx_msg_counter;   // counter of the reliable message awaiting ack
  int      tx_attempts;      // transmissions performed (1 after first send)
  uint32_t tx_next_ms;       // absolute time of next retransmission
  uint32_t base_ms;          // retransmit base interval

  // duplicate / replay detection (sliding 32-counter window below rx_max)
  bool     have_rx;
  uint32_t rx_max;
  uint32_t rx_window;
} mtrc_mrp_ctx;

void mtrc_mrp_init(mtrc_mrp_ctx *c, uint32_t base_ms /*0 -> default*/);

// Register an outbound frame just sent. If `reliable`, it is stored for
// retransmission and the first backoff deadline is armed. Returns 1 ok.
int  mtrc_mrp_on_send(mtrc_mrp_ctx *c, const uint8_t *frame, size_t len,
                      uint32_t msg_counter, bool reliable, uint32_t now_ms);

// Process an inbound ack counter. If it matches the pending message, the
// pending transmission is cleared. Returns 1 if it cleared a pending tx.
int  mtrc_mrp_on_ack(mtrc_mrp_ctx *c, uint32_t ack_counter);

// Duplicate detection for an inbound message counter. Returns 1 if this
// counter was already seen (duplicate); otherwise records it, returns 0.
int  mtrc_mrp_is_duplicate(mtrc_mrp_ctx *c, uint32_t msg_counter);

// Drive retransmission timers. If a retransmit is due, copies the stored
// frame into out[] (<= cap) and returns MTRC_MRP_RETRANSMIT. If the max
// transmission count is exceeded, clears pending and returns TIMEOUT.
mtrc_mrp_tick_result mtrc_mrp_tick(mtrc_mrp_ctx *c, uint32_t now_ms,
                                   uint8_t *out, size_t cap, size_t *out_len);

// Build a Secure Channel Standalone Acknowledgement frame (no payload).
// Returns frame length or -1.
int  mtrc_mrp_build_ack(uint8_t *out, size_t cap,
                        uint16_t session_id, uint32_t local_counter,
                        uint16_t exchange_id, bool initiator,
                        uint32_t ack_counter);

#ifdef __cplusplus
}
#endif

#endif // MTRC_MRP_H
