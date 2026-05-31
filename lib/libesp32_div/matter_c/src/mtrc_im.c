// mtrc_im.c — Matter Interaction Model subset. See mtrc_im.h. GPLv3.

#include "mtrc_im.h"
#include "mtrc_tlv.h"

int mtrc_im_parse_first_command(const uint8_t *buf, size_t len,
                                uint16_t *endpoint, uint32_t *cluster,
                                uint32_t *command) {
  // The only TLV list in an InvokeRequest is the CommandPath
  // {0:endpoint, 1:cluster, 2:command}. Scan for it.
  mtrc_tlv_reader r; mtrc_tlv_reader_init(&r, buf, len);
  mtrc_tlv_elem e;
  while (mtrc_tlv_read(&r, &e)) {
    if (e.type != MTRC_TLV_LIST) continue;
    uint16_t ep = 0; uint32_t cl = 0, cmd = 0; int have = 0;
    mtrc_tlv_elem ie;
    while (mtrc_tlv_read(&r, &ie) && ie.type != MTRC_TLV_END) {
      if (ie.tag.ctrl != MTRC_TLV_TAG_CONTEXT) continue;
      if      (ie.tag.number == 0) { ep  = (uint16_t)ie.u; have |= 1; }
      else if (ie.tag.number == 1) { cl  = (uint32_t)ie.u; have |= 2; }
      else if (ie.tag.number == 2) { cmd = (uint32_t)ie.u; have |= 4; }
    }
    if (have == 7) { *endpoint = ep; *cluster = cl; *command = cmd; return 1; }
  }
  return 0;
}

int mtrc_im_parse_first_attribute(const uint8_t *buf, size_t len,
                                  uint16_t *endpoint, uint32_t *cluster,
                                  uint32_t *attribute) {
  // AttributePathIB is the only list in a ReadRequest: {2:ep,3:cluster,4:attr}.
  mtrc_tlv_reader r; mtrc_tlv_reader_init(&r, buf, len);
  mtrc_tlv_elem e;
  while (mtrc_tlv_read(&r, &e)) {
    if (e.type != MTRC_TLV_LIST) continue;
    uint16_t ep = 0; uint32_t cl = 0, at = 0; int have = 0;
    mtrc_tlv_elem ie;
    while (mtrc_tlv_read(&r, &ie) && ie.type != MTRC_TLV_END) {
      if (ie.tag.ctrl != MTRC_TLV_TAG_CONTEXT) continue;
      if      (ie.tag.number == 2) { ep = (uint16_t)ie.u; }
      else if (ie.tag.number == 3) { cl = (uint32_t)ie.u; have |= 1; }
      else if (ie.tag.number == 4) { at = (uint32_t)ie.u; have |= 2; }
    }
    if (have == 3) { *endpoint = ep; *cluster = cl; *attribute = at; return 1; }
  }
  return 0;
}

int mtrc_im_parse_subscribe(const uint8_t *buf, size_t len,
                            uint16_t *endpoint, uint32_t *cluster,
                            uint32_t *attribute, uint16_t *max_interval) {
  // MaxIntervalCeiling = top-level ctx2 (u16); attribute path = AttributePathIB.
  *max_interval = 0;
  mtrc_tlv_reader r; mtrc_tlv_reader_init(&r, buf, len);
  mtrc_tlv_elem e;
  if (mtrc_tlv_read(&r, &e) && e.type == MTRC_TLV_STRUCT) {
    int depth = 1; mtrc_tlv_elem se;
    while (depth == 1 && mtrc_tlv_read(&r, &se)) {
      if (se.type == MTRC_TLV_END) break;
      if (se.tag.ctrl == MTRC_TLV_TAG_CONTEXT && se.tag.number == 2 &&
          se.type == MTRC_TLV_UINT) { *max_interval = (uint16_t)se.u; }
      if (se.type==MTRC_TLV_STRUCT||se.type==MTRC_TLV_ARRAY||se.type==MTRC_TLV_LIST) {
        int d=1; mtrc_tlv_elem x;
        while (d>0 && mtrc_tlv_read(&r,&x)) {
          if (x.type==MTRC_TLV_STRUCT||x.type==MTRC_TLV_ARRAY||x.type==MTRC_TLV_LIST) d++;
          else if (x.type==MTRC_TLV_END) d--;
        }
      }
    }
  }
  return mtrc_im_parse_first_attribute(buf, len, endpoint, cluster, attribute);
}

int mtrc_im_build_subscribe_response(uint8_t *out, size_t cap,
                                     uint32_t sub_id, uint16_t max_interval) {
  mtrc_tlv_writer w; mtrc_tlv_writer_init(&w, out, cap);
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());          // SubscribeResponseMessage
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0), sub_id);      // SubscriptionId
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(2), max_interval);// MaxInterval
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0xFF), 1);        // interactionModelRevision
  mtrc_tlv_end_container(&w);
  return mtrc_tlv_writer_ok(&w) ? (int)mtrc_tlv_writer_len(&w) : -1;
}

int mtrc_im_build_report_uint(uint8_t *out, size_t cap, uint32_t sub_id,
                              uint16_t endpoint, uint32_t cluster,
                              uint32_t attribute, uint64_t value) {
  mtrc_tlv_writer w; mtrc_tlv_writer_init(&w, out, cap);
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());          // ReportDataMessage
  if (sub_id) mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0), sub_id);   // SubscriptionId
  mtrc_tlv_start_array(&w, mtrc_tlv_ctx(1));           // AttributeReports
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());          //  AttributeReportIB
  mtrc_tlv_start_struct(&w, mtrc_tlv_ctx(1));          //   AttributeDataIB
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0), 1);           //    DataVersion
  mtrc_tlv_start_list(&w, mtrc_tlv_ctx(1));            //    AttributePathIB
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(2), endpoint);
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(3), cluster);
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(4), attribute);
  mtrc_tlv_end_container(&w);                          //    end path
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(2), value);       //    Data (value)
  mtrc_tlv_end_container(&w);                          //   end AttributeDataIB
  mtrc_tlv_end_container(&w);                          //  end AttributeReportIB
  mtrc_tlv_end_container(&w);                          // end AttributeReports
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0xFF), 1);        // interactionModelRevision
  mtrc_tlv_end_container(&w);                          // end message
  return mtrc_tlv_writer_ok(&w) ? (int)mtrc_tlv_writer_len(&w) : -1;
}

// Shared opener: emit the ReportData envelope down to the AttributePathIB,
// leaving the writer positioned to append the Data value. `w` is initialized.
static void report_open(mtrc_tlv_writer *w, uint8_t *out, size_t cap, uint32_t sub_id,
                        uint16_t endpoint, uint32_t cluster, uint32_t attribute) {
  mtrc_tlv_writer_init(w, out, cap);
  mtrc_tlv_start_struct(w, mtrc_tlv_anon());           // ReportDataMessage
  if (sub_id) mtrc_tlv_put_uint(w, mtrc_tlv_ctx(0), sub_id);   // SubscriptionId
  mtrc_tlv_start_array(w, mtrc_tlv_ctx(1));            // AttributeReports
  mtrc_tlv_start_struct(w, mtrc_tlv_anon());           //  AttributeReportIB
  mtrc_tlv_start_struct(w, mtrc_tlv_ctx(1));           //   AttributeDataIB
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(0), 1);            //    DataVersion
  mtrc_tlv_start_list(w, mtrc_tlv_ctx(1));             //    AttributePathIB
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(2), endpoint);
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(3), cluster);
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(4), attribute);
  mtrc_tlv_end_container(w);                           //    end path
  // caller appends Data at ctx2 here
}

// Shared closer: end AttributeDataIB/ReportIB/array + interactionModelRevision.
static int report_close(mtrc_tlv_writer *w) {
  mtrc_tlv_end_container(w);                           //   end AttributeDataIB
  mtrc_tlv_end_container(w);                           //  end AttributeReportIB
  mtrc_tlv_end_container(w);                           // end AttributeReports
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(0xFF), 1);         // interactionModelRevision
  mtrc_tlv_end_container(w);                           // end message
  return mtrc_tlv_writer_ok(w) ? (int)mtrc_tlv_writer_len(w) : -1;
}

int mtrc_im_build_report_list_uint(uint8_t *out, size_t cap, uint32_t sub_id,
                                   uint16_t endpoint, uint32_t cluster,
                                   uint32_t attribute,
                                   const uint32_t *vals, int count) {
  mtrc_tlv_writer w;
  report_open(&w, out, cap, sub_id, endpoint, cluster, attribute);
  mtrc_tlv_start_array(&w, mtrc_tlv_ctx(2));           //    Data = array
  for (int i = 0; i < count; i++)
    mtrc_tlv_put_uint(&w, mtrc_tlv_anon(), vals ? vals[i] : 0);
  mtrc_tlv_end_container(&w);                          //    end array
  return report_close(&w);
}

int mtrc_im_build_report_devtypelist(uint8_t *out, size_t cap, uint32_t sub_id,
                                     uint16_t endpoint, uint32_t cluster,
                                     uint32_t attribute,
                                     uint32_t device_type, uint16_t revision) {
  mtrc_tlv_writer w;
  report_open(&w, out, cap, sub_id, endpoint, cluster, attribute);
  mtrc_tlv_start_array(&w, mtrc_tlv_ctx(2));           //    Data = array
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());          //     DeviceTypeStruct
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0), device_type); //      0: deviceType
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(1), revision);    //      1: revision
  mtrc_tlv_end_container(&w);                          //     end struct
  mtrc_tlv_end_container(&w);                          //    end array
  return report_close(&w);
}

int mtrc_im_build_cmd_response_u8(uint8_t *out, size_t cap,
                                  uint16_t endpoint, uint32_t cluster,
                                  uint32_t resp_command, uint8_t field0) {
  mtrc_tlv_writer w; mtrc_tlv_writer_init(&w, out, cap);
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());          // InvokeResponseMessage
  mtrc_tlv_put_bool (&w, mtrc_tlv_ctx(0), false);      // suppressResponse
  mtrc_tlv_start_array(&w, mtrc_tlv_ctx(1));           // InvokeResponses
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());          //  InvokeResponseIB
  mtrc_tlv_start_struct(&w, mtrc_tlv_ctx(0));          //   command = CommandDataIB
  mtrc_tlv_start_list(&w, mtrc_tlv_ctx(0));            //    CommandPath (list)
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0), endpoint);
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(1), cluster);
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(2), resp_command);
  mtrc_tlv_end_container(&w);                          //    end CommandPath
  mtrc_tlv_start_struct(&w, mtrc_tlv_ctx(1));          //    CommandFields
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0), field0);      //     0: errorCode
  mtrc_tlv_put_utf8(&w, mtrc_tlv_ctx(1), "", 0);       //     1: debugText ""
  mtrc_tlv_end_container(&w);                          //    end CommandFields
  mtrc_tlv_end_container(&w);                          //   end CommandDataIB
  mtrc_tlv_end_container(&w);                          //  end InvokeResponseIB
  mtrc_tlv_end_container(&w);                          // end InvokeResponses
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0xFF), 1);        // interactionModelRevision
  mtrc_tlv_end_container(&w);                          // end message
  return mtrc_tlv_writer_ok(&w) ? (int)mtrc_tlv_writer_len(&w) : -1;
}

int mtrc_im_build_status(uint8_t *out, size_t cap,
                         uint16_t endpoint, uint32_t cluster, uint32_t command,
                         uint8_t status) {
  mtrc_tlv_writer w; mtrc_tlv_writer_init(&w, out, cap);
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());          // InvokeResponseMessage
  mtrc_tlv_put_bool (&w, mtrc_tlv_ctx(0), false);      // suppressResponse
  mtrc_tlv_start_array(&w, mtrc_tlv_ctx(1));           // InvokeResponses
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());          //  InvokeResponseIB
  mtrc_tlv_start_struct(&w, mtrc_tlv_ctx(1));          //   status = CommandStatusIB
  mtrc_tlv_start_list(&w, mtrc_tlv_ctx(0));            //    CommandPath
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0), endpoint);
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(1), cluster);
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(2), command);
  mtrc_tlv_end_container(&w);                          //    end CommandPath
  mtrc_tlv_start_struct(&w, mtrc_tlv_ctx(1));          //    StatusIB
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0), status);      //     0: status
  mtrc_tlv_end_container(&w);                          //    end StatusIB
  mtrc_tlv_end_container(&w);                          //   end CommandStatusIB
  mtrc_tlv_end_container(&w);                          //  end InvokeResponseIB
  mtrc_tlv_end_container(&w);                          // end InvokeResponses
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0xFF), 1);        // interactionModelRevision
  mtrc_tlv_end_container(&w);                          // end message
  return mtrc_tlv_writer_ok(&w) ? (int)mtrc_tlv_writer_len(&w) : -1;
}
