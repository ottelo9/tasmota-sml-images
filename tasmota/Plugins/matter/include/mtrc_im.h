// mtrc_im.h — Matter Interaction Model (subset) for matter_c.
//
// Just enough of the IM (Core Spec §10) to drive commissioning:
// parse an InvokeRequest's first command path, and build InvokeResponses
// (a command response, or a status). Protocol id = 0x0001 (IM).
//
//   InvokeRequestMessage = struct {
//     0:suppressResponse(bool) 1:timedRequest(bool)
//     2:InvokeRequests = array[ CommandDataIB struct {
//          0:CommandPath = list{0:endpoint 1:cluster 2:command}
//          1:CommandFields = struct{...} } ]
//     0xFF:interactionModelRevision(u8) }
//   InvokeResponseMessage = struct {
//     0:suppressResponse(bool)
//     1:InvokeResponses = array[ InvokeResponseIB struct {
//          0:command = CommandDataIB{ 0:CommandPath list, 1:CommandFields }
//          | 1:status = CommandStatusIB{ 0:CommandPath, 1:StatusIB{0:status} } } ]
//     0xFF:interactionModelRevision(u8) }
//
// GPLv3. Implemented from the Matter spec.

#ifndef MTRC_IM_H
#define MTRC_IM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// IM protocol opcodes (Core Spec §4.4.3 / §10)
#define MTRC_IM_STATUS_RESPONSE     0x01
#define MTRC_IM_READ_REQUEST        0x02
#define MTRC_IM_SUBSCRIBE_REQUEST   0x03
#define MTRC_IM_SUBSCRIBE_RESPONSE  0x04
#define MTRC_IM_REPORT_DATA         0x05
#define MTRC_IM_WRITE_REQUEST       0x06
#define MTRC_IM_WRITE_RESPONSE      0x07
#define MTRC_IM_INVOKE_REQUEST      0x08
#define MTRC_IM_INVOKE_RESPONSE     0x09
#define MTRC_IM_TIMED_REQUEST       0x0A

// Parse the first command path out of an InvokeRequest payload (CommandPath
// list {0:endpoint,1:cluster,2:command}). Returns 1, fills out, or 0.
int mtrc_im_parse_first_command(const uint8_t *buf, size_t len,
                                uint16_t *endpoint, uint32_t *cluster,
                                uint32_t *command);

// Parse the first attribute path out of a ReadRequest payload
// (AttributePathIB list {2:endpoint,3:cluster,4:attribute}; endpoint may be
// wildcard -> defaults to 0). Returns 1, fills out, or 0.
int mtrc_im_parse_first_attribute(const uint8_t *buf, size_t len,
                                  uint16_t *endpoint, uint32_t *cluster,
                                  uint32_t *attribute);

// Build a ReportDataMessage with a single unsigned-int attribute value
// (covers bool/enum/u8..u32). If sub_id != 0 a SubscriptionId field is
// added (for subscription reports). Returns length, or -1.
int mtrc_im_build_report_uint(uint8_t *out, size_t cap, uint32_t sub_id,
                              uint16_t endpoint, uint32_t cluster,
                              uint32_t attribute, uint64_t value);

// Build a ReportDataMessage whose attribute value is an ARRAY of unsigned
// ints (covers Descriptor ServerList / PartsList / ClientList — lists of
// cluster or endpoint ids). Returns length, or -1.
int mtrc_im_build_report_list_uint(uint8_t *out, size_t cap, uint32_t sub_id,
                                   uint16_t endpoint, uint32_t cluster,
                                   uint32_t attribute,
                                   const uint32_t *vals, int count);

// Build a ReportDataMessage for the Descriptor DeviceTypeList attribute: an
// array of DeviceTypeStruct{ 0:deviceType(u32), 1:revision(u16) }. Emits a
// single entry. Returns length, or -1.
int mtrc_im_build_report_devtypelist(uint8_t *out, size_t cap, uint32_t sub_id,
                                     uint16_t endpoint, uint32_t cluster,
                                     uint32_t attribute,
                                     uint32_t device_type, uint16_t revision);

// Parse a SubscribeRequest: first attribute path + MaxIntervalCeiling (ctx2).
int mtrc_im_parse_subscribe(const uint8_t *buf, size_t len,
                            uint16_t *endpoint, uint32_t *cluster,
                            uint32_t *attribute, uint16_t *max_interval);

// Build a SubscribeResponseMessage {0:SubscriptionId, 2:MaxInterval}.
int mtrc_im_build_subscribe_response(uint8_t *out, size_t cap,
                                     uint32_t sub_id, uint16_t max_interval);

// Build an InvokeResponseMessage carrying a single command response whose
// fields are { 0: status_field0 (u8), 1: "" } — the shape of the General
// Commissioning *Response commands ({errorCode, debugText}). Returns length.
int mtrc_im_build_cmd_response_u8(uint8_t *out, size_t cap,
                                  uint16_t endpoint, uint32_t cluster,
                                  uint32_t resp_command, uint8_t field0);

// Build an InvokeResponseMessage carrying a CommandStatusIB (status only),
// e.g. for unsupported commands. `status` is the IM StatusIB status code.
int mtrc_im_build_status(uint8_t *out, size_t cap,
                         uint16_t endpoint, uint32_t cluster, uint32_t command,
                         uint8_t status);

#ifdef __cplusplus
}
#endif

#endif // MTRC_IM_H
