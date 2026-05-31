// mtrc_cert.c — Matter operational certificate TLV parser. See mtrc_cert.h.
// GPLv3.

#include "mtrc_cert.h"
#include "mtrc_tlv.h"
#include <string.h>

// Cert element tags
enum { T_SERIAL=1, T_SIGALGO=2, T_ISSUER=3, T_NOTBEFORE=4, T_NOTAFTER=5,
       T_SUBJECT=6, T_PKALGO=7, T_CURVE=8, T_PUBKEY=9, T_EXT=10, T_SIG=11 };
// DN attribute tags (Matter-specific)
enum { DN_NODE=17, DN_ICAC=19, DN_RCAC=20, DN_FABRIC=21, DN_NOCCAT=22 };
// Extension tags
enum { X_BASIC=1, X_KEYUSAGE=2, X_BASIC_ISCA=1 };

// Read a DN list (caller already consumed the list header). Extracts the
// Matter id attributes into the cert, choosing subject vs issuer fields.
static void read_dn(mtrc_tlv_reader *r, mtrc_cert *c, int is_subject) {
  mtrc_tlv_elem e;
  while (mtrc_tlv_read(r, &e) && e.type != MTRC_TLV_END) {
    if (e.tag.ctrl != MTRC_TLV_TAG_CONTEXT) continue;
    switch (e.tag.number) {
      case DN_NODE:   if (is_subject) { c->subject_node_id=e.u; c->have_node_id=true; } break;
      case DN_FABRIC: if (is_subject) { c->subject_fabric_id=e.u; c->have_fabric_id=true; } break;
      case DN_RCAC:
        if (is_subject) { c->subject_rcac_id=e.u; c->have_rcac_id=true; }
        else            { c->issuer_rcac_id=e.u;  c->issuer_has_rcac=true; }
        break;
      case DN_ICAC:
        if (is_subject) { c->subject_icac_id=e.u; c->have_icac_id=true; }
        else            { c->issuer_icac_id=e.u;  c->issuer_has_icac=true; }
        break;
      default: break;   // common-name etc. ignored
    }
  }
}

// Read the extensions list (header already consumed): pull basicConstraints.isCA.
static void read_ext(mtrc_tlv_reader *r, mtrc_cert *c) {
  mtrc_tlv_elem e;
  while (mtrc_tlv_read(r, &e) && e.type != MTRC_TLV_END) {
    if (e.type == MTRC_TLV_STRUCT && e.tag.ctrl == MTRC_TLV_TAG_CONTEXT
        && e.tag.number == X_BASIC) {
      mtrc_tlv_elem ie;
      while (mtrc_tlv_read(r, &ie) && ie.type != MTRC_TLV_END) {
        if (ie.type == MTRC_TLV_BOOL && ie.tag.ctrl == MTRC_TLV_TAG_CONTEXT
            && ie.tag.number == X_BASIC_ISCA)
          c->is_ca = (ie.u != 0);
      }
    } else if (e.type == MTRC_TLV_STRUCT || e.type == MTRC_TLV_ARRAY ||
               e.type == MTRC_TLV_LIST) {
      // skip other nested extensions
      int depth = 1;
      mtrc_tlv_elem se;
      while (depth > 0 && mtrc_tlv_read(r, &se)) {
        if (se.type==MTRC_TLV_STRUCT||se.type==MTRC_TLV_ARRAY||se.type==MTRC_TLV_LIST) depth++;
        else if (se.type==MTRC_TLV_END) depth--;
      }
    }
  }
}

int mtrc_cert_parse(const uint8_t *tlv, size_t len, mtrc_cert *out) {
  if (!tlv || !out) return 0;
  memset(out, 0, sizeof(*out));
  mtrc_tlv_reader r; mtrc_tlv_reader_init(&r, tlv, len);
  mtrc_tlv_elem e;
  if (!mtrc_tlv_read(&r, &e) || e.type != MTRC_TLV_STRUCT) return 0;  // outer struct

  while (mtrc_tlv_read(&r, &e) && e.type != MTRC_TLV_END) {
    uint32_t tag = e.tag.number;
    switch (tag) {
      case T_SERIAL:
        if (e.type==MTRC_TLV_BYTES && e.bytes_len<=sizeof(out->serial)) {
          memcpy(out->serial, e.bytes, e.bytes_len); out->serial_len=(uint8_t)e.bytes_len;
        } break;
      case T_NOTBEFORE: out->not_before=(uint32_t)e.u; break;
      case T_NOTAFTER:  out->not_after=(uint32_t)e.u;  break;
      case T_ISSUER:    if (e.type==MTRC_TLV_LIST) read_dn(&r, out, 0); break;
      case T_SUBJECT:   if (e.type==MTRC_TLV_LIST) read_dn(&r, out, 1); break;
      case T_PUBKEY:
        if (e.type==MTRC_TLV_BYTES && e.bytes_len==65) {
          memcpy(out->pubkey, e.bytes, 65); out->have_pubkey=true;
        } break;
      case T_SIG:
        if (e.type==MTRC_TLV_BYTES && e.bytes_len==64) {
          memcpy(out->signature, e.bytes, 64); out->have_sig=true;
        } break;
      case T_EXT:       if (e.type==MTRC_TLV_LIST) read_ext(&r, out); break;
      default:
        // skip any unexpected nested container
        if (e.type==MTRC_TLV_STRUCT||e.type==MTRC_TLV_ARRAY||e.type==MTRC_TLV_LIST) {
          int depth=1; mtrc_tlv_elem se;
          while (depth>0 && mtrc_tlv_read(&r,&se)) {
            if (se.type==MTRC_TLV_STRUCT||se.type==MTRC_TLV_ARRAY||se.type==MTRC_TLV_LIST) depth++;
            else if (se.type==MTRC_TLV_END) depth--;
          }
        }
        break;
    }
  }
  return r.err ? 0 : (out->have_pubkey ? 1 : 0);
}

int mtrc_cert_chain_check(const mtrc_cert *noc, const mtrc_cert *icac,
                          uint64_t fabric_id, uint32_t now_epoch) {
  if (!noc) return 0;

  // --- NOC: must be a leaf operational cert bound to the matched fabric -------
  if (noc->is_ca) return 0;                              // a NOC is never a CA
  if (!noc->have_node_id || !noc->have_fabric_id) return 0;  // NOC carries both ids
  if (fabric_id && noc->subject_fabric_id != fabric_id) return 0;  // our fabric

  // --- Intermediate (if the chain supplies one) -------------------------------
  if (icac) {
    if (!icac->is_ca) return 0;                          // an ICAC must be a CA
    // If both name the ICAC id, they must agree (NOC.issuer == ICAC.subject).
    if (noc->issuer_has_icac && icac->have_icac_id &&
        noc->issuer_icac_id != icac->subject_icac_id) return 0;
    // If the ICAC constrains a fabric, it must be the NOC's fabric.
    if (icac->have_fabric_id &&
        icac->subject_fabric_id != noc->subject_fabric_id) return 0;
  }

  // --- Validity window — only when the caller has a trusted clock --------------
  // not_before / not_after == 0 means "unbounded" in that direction.
  if (now_epoch) {
    if (noc->not_before && now_epoch < noc->not_before) return 0;
    if (noc->not_after  && now_epoch > noc->not_after)  return 0;
    if (icac) {
      if (icac->not_before && now_epoch < icac->not_before) return 0;
      if (icac->not_after  && now_epoch > icac->not_after)  return 0;
    }
  }
  return 1;
}
