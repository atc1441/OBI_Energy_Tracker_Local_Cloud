#!/usr/bin/env python3
"""
gen_certs.py -- one-time PKI setup for the local OBI cloud.

Creates a self-signed CA and, signed by it:
  - a TLS **server** cert (for mqtts_server.py) with the given host in its SAN,
  - a **claim** client cert (the short-lived cert the device gets over BLE),
  - a **permanent** client cert (the "consistent" cert the server hands back during
    fleet-provisioning; the device reconnects with it).

It also writes `ble_config.json` = the `SetTMPCertificateRequest.data` that ble_provision.py
sends to the device: our CA as `caPem`, the claim cert/key, and `url = mqtts://<host>`.

The device trusts our server because it verifies the server cert against `caPem` (= our CA).

Usage:
  python gen_certs.py                      # host auto-detected (LAN IP)
  python gen_certs.py --host 192.168.1.152 --out pki

Needs: pip install cryptography
"""
from __future__ import annotations
import argparse, datetime, ipaddress, json, os, socket, sys

try:
    from cryptography import x509
    from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
except ImportError:
    sys.exit("needs 'cryptography' -- pip install cryptography")

UTC = datetime.timezone.utc


def lan_ip() -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


def _key():
    return rsa.generate_private_key(public_exponent=65537, key_size=2048)


def _key_pem(k) -> str:
    return k.private_bytes(serialization.Encoding.PEM,
                           serialization.PrivateFormat.TraditionalOpenSSL,
                           serialization.NoEncryption()).decode()


def _cert_pem(c) -> str:
    return c.public_bytes(serialization.Encoding.PEM).decode()


def _name(cn):
    return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)])


def make_ca():
    k = _key()
    subj = _name("OBI Local CA")
    now = datetime.datetime.now(UTC)
    c = (x509.CertificateBuilder()
         .subject_name(subj).issuer_name(subj).public_key(k.public_key())
         .serial_number(x509.random_serial_number())
         .not_valid_before(now - datetime.timedelta(days=1))
         .not_valid_after(now + datetime.timedelta(days=3650))
         .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
         .add_extension(x509.KeyUsage(key_cert_sign=True, crl_sign=True, digital_signature=True,
                                      content_commitment=False, key_encipherment=False,
                                      data_encipherment=False, key_agreement=False,
                                      encipher_only=False, decipher_only=False), critical=True)
         .add_extension(x509.SubjectKeyIdentifier.from_public_key(k.public_key()), critical=False)
         .sign(k, hashes.SHA256()))
    return k, c


def make_cert(ca_key, ca_cert, cn, sans=None, server=False):
    k = _key()
    now = datetime.datetime.now(UTC)
    b = (x509.CertificateBuilder()
         .subject_name(_name(cn)).issuer_name(ca_cert.subject).public_key(k.public_key())
         .serial_number(x509.random_serial_number())
         .not_valid_before(now - datetime.timedelta(days=1))
         .not_valid_after(now + datetime.timedelta(days=3650))
         .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True))
    if sans:
        alt = []
        for s in sans:
            try:
                ip = ipaddress.ip_address(s)
                alt.append(x509.IPAddress(ip))
                alt.append(x509.DNSName(s))   # also as DNS: some mbedTLS builds match IPs as strings
            except ValueError:
                alt.append(x509.DNSName(s))
        b = b.add_extension(x509.SubjectAlternativeName(alt), critical=False)
    eku = ExtendedKeyUsageOID.SERVER_AUTH if server else ExtendedKeyUsageOID.CLIENT_AUTH
    b = b.add_extension(x509.ExtendedKeyUsage([eku]), critical=False)
    b = b.add_extension(x509.KeyUsage(digital_signature=True, key_encipherment=True,
                                      content_commitment=False, data_encipherment=False,
                                      key_agreement=False, key_cert_sign=False, crl_sign=False,
                                      encipher_only=False, decipher_only=False), critical=False)
    b = b.add_extension(x509.SubjectKeyIdentifier.from_public_key(k.public_key()), critical=False)
    b = b.add_extension(
        x509.AuthorityKeyIdentifier.from_issuer_public_key(ca_cert.public_key()), critical=False)
    return k, b.sign(ca_key, hashes.SHA256())


def write(path, text):
    with open(path, "w", newline="\n") as f:
        f.write(text if text.endswith("\n") else text + "\n")


def main():
    ap = argparse.ArgumentParser(description="one-time PKI for the local OBI cloud")
    ap.add_argument("--host", default=None, help="server host/IP the device will connect to (default: LAN IP)")
    ap.add_argument("--port", type=int, default=8883)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "pki"))
    ap.add_argument("--template", default="TrustedUserProvTemplEnergyTracking")
    ap.add_argument("--thing-cn", default=None, help="CN for the claim cert (default: random 64-hex)")
    args = ap.parse_args()

    host = args.host or lan_ip()
    os.makedirs(args.out, exist_ok=True)
    cn = args.thing_cn or os.urandom(32).hex()

    ca_key, ca = make_ca()
    sans = [host, "127.0.0.1", "localhost"]
    srv_key, srv = make_cert(ca_key, ca, host, sans=sans, server=True)
    claim_key, claim = make_cert(ca_key, ca, cn)
    perm_key, perm = make_cert(ca_key, ca, cn)   # "consistent" cert handed back on provisioning

    files = {
        "ca.pem": _cert_pem(ca), "ca.key": _key_pem(ca_key),
        "server.crt": _cert_pem(srv), "server.key": _key_pem(srv_key),
        "claim.crt": _cert_pem(claim), "claim.key": _key_pem(claim_key),
        "permanent.crt": _cert_pem(perm), "permanent.key": _key_pem(perm_key),
    }
    for name, text in files.items():
        write(os.path.join(args.out, name), text)

    url = f"mqtts://{host}" if args.port == 8883 else f"mqtts://{host}:{args.port}"
    ble_config = {
        "url": url,
        "provisioningTemplateName": args.template,
        "caPem": _cert_pem(ca),
        "certPem": _cert_pem(claim),
        "privateKey": _key_pem(claim_key),
    }
    write(os.path.join(args.out, "ble_config.json"), json.dumps(ble_config, indent=2))

    print(f"[+] wrote CA + server + claim + permanent certs to {args.out}")
    print(f"    server host/SAN : {host}  (also 127.0.0.1, localhost)")
    print(f"    claim/thing CN  : {cn}")
    print(f"    device url      : {url}")
    print(f"    ble_config.json : {os.path.join(args.out, 'ble_config.json')}")
    print(f"\n[next] start server:   python mqtts_server.py --host 0.0.0.0 --port {args.port}")
    print(f"[next] provision BLE:  python ble_provision.py --config {os.path.join(args.out,'ble_config.json')} "
          f"--key <32hex> --ssid <wifi> --password <pw>")


if __name__ == "__main__":
    main()
