#!/usr/bin/env python3
# Extract the CSA TEST attestation credentials (VID 0xFFF1 / PID 0x8000) from
# Tasmota's Berry Matter (lib/libesp32/berry_matter/.../Matter_Certs.be) and
# emit them as matter_c's gated attestation header (mtrc_attest_creds.h).
#
# These are the same DAC/PAI/CD that Tasmota presents to Apple Home / Google /
# Alexa: the DAC chains to the CSA test PAA and the CD is CSA-signed, so the
# commercial ecosystems accept VID 0xFFF1 test devices. Preferred over the
# self-signed gen_attest_creds.py for real-ecosystem pairing.
#
# Usage:  python3 gen_csa_creds.py   (writes ../include/mtrc_attest_creds.h)

import re, os

here = os.path.dirname(os.path.abspath(__file__))
src_path = os.path.join(here, "..", "..", "..",
                        "libesp32", "berry_matter", "src", "embedded", "Matter_Certs.be")
src = open(src_path).read()

def extract(name):
    m = re.search(r'static var %s\s*=\s*bytes\((.*?)\)' % re.escape(name), src, re.S)
    if not m:
        raise SystemExit("not found in Matter_Certs.be: " + name)
    return bytes.fromhex("".join(re.findall(r'"([0-9A-Fa-f]+)"', m.group(1))))

dac_priv = extract("DAC_Priv_FFF1_8000")
dac_der  = extract("DAC_Cert_FFF1_8000")
pai_der  = extract("PAI_Cert_FFF1")
cd       = extract("CD_FFF1_8000")
assert len(dac_priv) == 32, len(dac_priv)

def carr(name, b):
    body = ",".join("0x%02x" % x for x in b)
    return ("static const uint8_t %s[] = { %s };\n#define %s_LEN ((int)sizeof(%s))\n"
            % (name, body, name, name))

out = os.path.join(here, "..", "include", "mtrc_attest_creds.h")
with open(out, "w") as f:
    f.write("// mtrc_attest_creds.h — GENERATED from Tasmota Berry Matter's CSA\n"
            "// test attestation set (VID 0xFFF1 / PID 0x8000) via gen_csa_creds.py.\n"
            "// DAC chains to the CSA test PAA; CD is CSA-signed -> accepted by Apple\n"
            "// Home / Google / Alexa for test devices. Gated (MTRC_ATTEST_TEST_CREDS).\n"
            "#ifndef MTRC_ATTEST_CREDS_H\n#define MTRC_ATTEST_CREDS_H\n#include <stdint.h>\n\n")
    f.write(carr("MTRC_DAC_PRIV", dac_priv))
    f.write(carr("MTRC_DAC_DER",  dac_der))
    f.write(carr("MTRC_PAI_DER",  pai_der))
    f.write(carr("MTRC_CD",       cd))
    f.write("\n#endif\n")
print("wrote %s\n  DAC priv %d, DAC der %d, PAI der %d, CD %d (CSA test set FFF1/8000)"
      % (out, len(dac_priv), len(dac_der), len(pai_der), len(cd)))
