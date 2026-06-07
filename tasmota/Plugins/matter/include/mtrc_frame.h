// mtrc_frame.h — Matter message + protocol header codec for matter_c.
//
// Matter message layout (Core Spec §4.4), unsecured-session path:
//
//   Message Header:
//     Message Flags   1B   ver(7..5) | S(4) | rsv(3..2) | DSIZ(1..0)
//     Session ID      2B   LE  (0 = unsecured session)
//     Security Flags  1B   P(7) | C(6) | MX(5) | rsv | SessionType(1..0)
//     Message Counter 4B   LE
//     Source Node ID  0/8B LE  (present if S flag)
//     Dest Node/Group 0/8/2B   (per DSIZ: 0=none,1=node64,2=group16)
//   Protocol (Payload) Header:
//     Exchange Flags  1B   I(0) A(1) R(2) SX(3) V(4)
//     Opcode          1B
//     Exchange ID     2B   LE
//     Protocol ID     2B   LE
//     Vendor ID       0/2B LE  (present if V flag)
//     Ack Counter     0/4B LE  (present if A flag)
//   Application Payload ...
//
// Secured (encrypted) sessions add a MIC and privacy; that wraps this in
// Phase 3. For PASE/MRP bring-up everything rides the unsecured session.
//
// GPLv3. Implemented from the Matter spec, not converted from Berry.

#ifndef MTRC_FRAME_H
#define MTRC_FRAME_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Protocol IDs (Core Spec §4.4.3.2)
#define MTRC_PROTO_SECURE_CHANNEL  0x0000
#define MTRC_PROTO_IM              0x0001
#define MTRC_PROTO_BDX             0x0002
#define MTRC_PROTO_UDC             0x0003

// Secure Channel opcodes (subset)
#define MTRC_SC_MSG_COUNTER_SYNC_REQ 0x00
#define MTRC_SC_MSG_COUNTER_SYNC_RSP 0x01
#define MTRC_SC_STANDALONE_ACK       0x10
#define MTRC_SC_PBKDF_PARAM_REQ      0x20
#define MTRC_SC_PBKDF_PARAM_RSP      0x21
#define MTRC_SC_PASE_PAKE1           0x22
#define MTRC_SC_PASE_PAKE2           0x23
#define MTRC_SC_PASE_PAKE3           0x24
#define MTRC_SC_CASE_SIGMA1          0x30
#define MTRC_SC_CASE_SIGMA2          0x31
#define MTRC_SC_CASE_SIGMA3          0x32
#define MTRC_SC_STATUS_REPORT        0x40

// Destination size field
typedef enum { MTRC_DSIZ_NONE = 0, MTRC_DSIZ_NODE = 1, MTRC_DSIZ_GROUP = 2 } mtrc_dsiz;

typedef struct {
  uint8_t  version;       // normally 0
  uint16_t session_id;    // 0 = unsecured session
  uint8_t  session_type;  // 0 = unicast, 1 = group
  bool     control;       // C flag (separate counter space)
  uint32_t msg_counter;
  bool     has_src;       // S flag
  uint64_t src_node_id;   // if has_src
  mtrc_dsiz dsiz;
  uint64_t dest_node_id;  // if dsiz == NODE
  uint16_t dest_group_id; // if dsiz == GROUP
} mtrc_msg_header;

typedef struct {
  bool     initiator;     // I
  bool     ack;           // A  (ack_counter present)
  bool     reliability;   // R  (peer must acknowledge)
  bool     has_vendor;    // V
  uint8_t  opcode;
  uint16_t exchange_id;
  uint16_t protocol_id;
  uint16_t vendor_id;     // if has_vendor
  uint32_t ack_counter;   // if ack
} mtrc_proto_header;

// Encode message header + protocol header + payload into out[].
// Returns total byte length, or -1 on overflow/invalid.
int mtrc_frame_encode(uint8_t *out, size_t cap,
                      const mtrc_msg_header *mh, const mtrc_proto_header *ph,
                      const uint8_t *payload, size_t payload_len);

// Decode. On success fills mh/ph, points *payload at the app payload inside
// buf, sets *payload_len, returns total bytes consumed (== len). -1 on error.
int mtrc_frame_decode(const uint8_t *buf, size_t len,
                      mtrc_msg_header *mh, mtrc_proto_header *ph,
                      const uint8_t **payload, size_t *payload_len);

// ---- split codecs (used by the secured-message path: the message header
//      is the AAD, the protocol header + payload are the encrypted body) --

// The Security Flags byte for this header (P/C/MX/SessionType). Also the
// first byte of the AES-CCM nonce.
uint8_t mtrc_frame_security_flags(const mtrc_msg_header *mh);

// Encode only the message header. Returns its length, or -1.
int mtrc_frame_encode_msg_header(uint8_t *out, size_t cap, const mtrc_msg_header *mh);
// Decode only the message header. Returns header length consumed, or -1.
int mtrc_frame_decode_msg_header(const uint8_t *buf, size_t len, mtrc_msg_header *mh);

// Encode protocol header + payload. Returns length, or -1.
int mtrc_frame_encode_proto(uint8_t *out, size_t cap, const mtrc_proto_header *ph,
                            const uint8_t *payload, size_t payload_len);
// Decode protocol header + payload from a (decrypted) buffer. Returns 1/0.
int mtrc_frame_decode_proto(const uint8_t *buf, size_t len, mtrc_proto_header *ph,
                            const uint8_t **payload, size_t *payload_len);

#ifdef __cplusplus
}
#endif

#endif // MTRC_FRAME_H
