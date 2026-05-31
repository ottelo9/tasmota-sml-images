#!/usr/bin/env python3
# Generate DEV attestation credentials for matter_c's A2 (Device Attestation):
# a PAA -> PAI -> DAC P-256 chain (X.509 DER) + the DAC private key + a CD
# placeholder, emitted as a C header (include/mtrc_attest_creds.h).
#
# DEV/TEST ONLY — gated behind MTRC_ATTEST_TEST_CREDS, never shipped. For a real
# chip-tool pairing, swap in the CSA development DAC/PAI/CD set (the device
# handlers are identical; only these bytes change). Fixed key scalars make the
# output reproducible.
#
# Usage:  python3 gen_attest_creds.py   (writes ../include/mtrc_attest_creds.h)

import datetime, os
from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.serialization import Encoding

def key(scalar_byte):
    return ec.derive_private_key(int.from_bytes(bytes([scalar_byte])*32, 'big'), ec.SECP256R1())

def cert(sub_cn, sub_key, iss_cn, iss_key, ca, serial):
    b = (x509.CertificateBuilder()
         .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, sub_cn)]))
         .issuer_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, iss_cn)]))
         .not_valid_before(datetime.datetime(2021, 1, 1))
         .not_valid_after(datetime.datetime(2099, 1, 1))
         .serial_number(serial)
         .public_key(sub_key.public_key())
         .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True))
    return b.sign(iss_key, hashes.SHA256())

paa = key(0x0A); pai = key(0x0B); dac = key(0x0C)
paa_c = cert("Matter Dev PAA", paa, "Matter Dev PAA", paa, True, 0x0A01)
pai_c = cert("Matter Dev PAI", pai, "Matter Dev PAA", paa, True, 0x0B01)
dac_c = cert("Matter Dev DAC", dac, "Matter Dev PAI", pai, False, 0x0C01)

dac_priv = dac.private_numbers().private_value.to_bytes(32, 'big')
dac_der  = dac_c.public_bytes(Encoding.DER)
pai_der  = pai_c.public_bytes(Encoding.DER)
cd = b'CSA-MATTER-DEV-CD-PLACEHOLDER--32'   # opaque to the prover; real CD = CMS

def carr(name, b):
    body = ",".join("0x%02x" % x for x in b)
    return ("static const uint8_t %s[] = { %s };\n#define %s_LEN ((int)sizeof(%s))\n"
            % (name, body, name, name))

here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, "..", "include", "mtrc_attest_creds.h")
with open(out, "w") as f:
    f.write("// mtrc_attest_creds.h — GENERATED dev attestation credentials.\n"
            "// gen_attest_creds.py output. DEV/TEST ONLY (MTRC_ATTEST_TEST_CREDS).\n"
            "// Self-generated PAA->PAI->DAC chain; swap for the CSA set for real\n"
            "// chip-tool. Do not ship. GPLv3.\n"
            "#ifndef MTRC_ATTEST_CREDS_H\n#define MTRC_ATTEST_CREDS_H\n#include <stdint.h>\n\n")
    f.write(carr("MTRC_DAC_PRIV", dac_priv))
    f.write(carr("MTRC_DAC_DER",  dac_der))
    f.write(carr("MTRC_PAI_DER",  pai_der))
    f.write(carr("MTRC_CD",       cd))
    f.write("\n#endif\n")
print("wrote %s  (DAC priv 32, DAC der %d, PAI der %d, CD %d)"
      % (out, len(dac_der), len(pai_der), len(cd)))
print("DAC pubkey:", dac.public_key().public_bytes(
    Encoding.X962, __import__('cryptography.hazmat.primitives.serialization',
    fromlist=['PublicFormat']).PublicFormat.UncompressedPoint).hex())
