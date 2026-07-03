#!/usr/bin/env python3
"""
mqtts_server.py -- a tiny local stand-in for the OBI / AWS-IoT MQTTS cloud.

A minimal MQTT 3.1.1 / 5.0 broker over TLS (mutual, permissive) that:
  * accepts the device using the certs we pushed over BLE (server cert chains to our CA),
  * answers AWS-IoT fleet-provisioning-by-claim:
      - PUBLISH $aws/certificates/create/json[/csr]  -> .../accepted { permanent cert }
      - PUBLISH $aws/provisioning-templates/<tpl>/provision/json -> .../accepted { thingName }
    (the "consistent" cert handed back is pki/permanent.crt -- reused every time),
  * logs every CONNECT / SUBSCRIBE / PUBLISH so you can see exactly what the device sends,
  * optionally pushes a shadow delta so the device receives something on reconnect.

Single device at a time; sequential connections (claim connect -> provision -> reconnect
with the permanent cert) are all handled. Run gen_certs.py first.

Usage:
  python mqtts_server.py --host 0.0.0.0 --port 8883
Needs: only the standard library.
"""
from __future__ import annotations
import argparse, hashlib, json, os, socket, ssl, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))


# ---------------- MQTT wire helpers ---------------- #
def enc_len(n: int) -> bytes:
    out = bytearray()
    while True:
        d = n % 128
        n //= 128
        if n:
            d |= 0x80
        out.append(d)
        if not n:
            break
    return bytes(out)


def rd_varint(b, i):
    mult, val = 1, 0
    while True:
        d = b[i]; i += 1
        val += (d & 0x7f) * mult
        if not d & 0x80:
            return val, i
        mult *= 128


def str16(s: str) -> bytes:
    b = s.encode()
    return len(b).to_bytes(2, "big") + b


def pkt(t, flags, payload=b"") -> bytes:
    return bytes([(t << 4) | flags]) + enc_len(len(payload)) + payload


def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        c = sock.recv(n - len(buf))
        if not c:
            return None
        buf += c
    return bytes(buf)


def recv_packet(sock):
    h = recv_exact(sock, 1)
    if not h:
        return None
    # remaining length varint from socket
    mult, val = 1, 0
    while True:
        d = recv_exact(sock, 1)
        if d is None:
            return None
        val += (d[0] & 0x7f) * mult
        if not d[0] & 0x80:
            break
        mult *= 128
    body = recv_exact(sock, val) if val else b""
    if body is None:
        return None
    return h[0] >> 4, h[0] & 0x0f, body


# ---------------- provisioning payloads ---------------- #
def load_pki(pki):
    def rd(n):
        return open(os.path.join(pki, n), encoding="utf-8").read()
    return {"perm_crt": rd("permanent.crt"), "perm_key": rd("permanent.key")}


def cert_response(pki):
    return json.dumps({
        "certificateId": os.urandom(32).hex(),
        "certificatePem": pki["perm_crt"],
        "privateKey": pki["perm_key"],
        "certificateOwnershipToken": "local-" + os.urandom(8).hex(),
    })


def provision_response(payload):
    thing = "obi-local-thing"
    try:
        params = json.loads(payload).get("parameters", {})
        thing = params.get("ThingName") or thing
    except Exception:
        pass
    return thing, json.dumps({"deviceConfiguration": {}, "thingName": thing})


# ---------------- connection handler ---------------- #
def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def fmt_payload(b: bytes) -> str:
    try:
        return json.dumps(json.loads(b.decode()), ensure_ascii=False)
    except Exception:
        try:
            return repr(b.decode())
        except Exception:
            return b.hex()


def handle(sock, addr, pki, push_shadow, set_interval=None, ota=None):
    v5 = False
    cid = "?"
    peer_cn = "?"
    sent = {"interval": False, "ota": False}
    try:
        cert = sock.getpeercert()
        if cert:
            for tup in cert.get("subject", ()):
                for k, v in tup:
                    if k == "commonName":
                        peer_cn = v
    except Exception:
        pass

    def publish_raw(t, raw):
        p = str16(t) + (b"\x00" if v5 else b"") + raw
        sock.sendall(pkt(3, 0, p))

    def publish(t, data):
        publish_raw(t, data.encode())
        log(f"  -> reply {t}")

    def send_interval(topic):
        # 1.0.x reads {sensor_upload_interval, session_id}; 1.2.x (multi-sensor) ALSO needs the target
        # sensor "uuid" (else "get uuid failed"). Extra fields are ignored by whichever firmware.
        secs, sess = set_interval
        body = {"sensor_upload_interval": secs, "session_id": sess}
        if "/sensor/" in topic:
            suid = topic.split("/sensor/", 1)[1].split("/", 1)[0]
            if suid and suid != "+":
                body["uuid"] = suid
        publish(topic, json.dumps(body))
        log(f"  -> SET upload interval = {secs}s (session_id {sess}) on {topic}")
        sent["interval"] = True

    try:
        while True:
            p = recv_packet(sock)
            if p is None:
                break
            ptype, flags, body = p

            if ptype == 1:  # CONNECT
                i = 0
                nlen = int.from_bytes(body[i:i+2], "big"); i += 2 + nlen
                level = body[i]; i += 1
                v5 = level == 5
                i += 1  # connect flags
                i += 2  # keepalive
                if v5:
                    plen, i = rd_varint(body, i); i += plen  # skip properties
                clen = int.from_bytes(body[i:i+2], "big"); i += 2
                cid = body[i:i+clen].decode("utf-8", "replace")
                connack = pkt(2, 0, b"\x00\x00\x00" if v5 else b"\x00\x00")
                sock.sendall(connack)
                log(f"CONNECT  client_id={cid!r}  cert_cn={peer_cn}  mqtt={'5.0' if v5 else '3.1.1'}  from {addr[0]}")

            elif ptype == 8:  # SUBSCRIBE
                pid = int.from_bytes(body[0:2], "big"); i = 2
                if v5:
                    plen, i = rd_varint(body, i); i += plen
                topics = []
                while i < len(body):
                    tl = int.from_bytes(body[i:i+2], "big"); i += 2
                    topics.append(body[i:i+tl].decode("utf-8", "replace")); i += tl
                    i += 1  # options byte
                suback = pkt(9, 0, pid.to_bytes(2, "big") + (b"\x00" if v5 else b"") + b"\x00" * len(topics))
                sock.sendall(suback)
                for t in topics:
                    log(f"SUBSCRIBE  {t}")
                    # downlink: once the device subscribes to its upload-interval command
                    # topic, push the change request straight back to it (proto2 cmd6).
                    if set_interval and "/sensor/" in t and "upload-interval-change-request" in t \
                            and not sent["interval"]:
                        # (only the /sensor/ topic -- 1.2.x also has an /outlet/ one we ignore)
                        if "/sensor/+/" in t:
                            # 1.2.x wildcard sub: defer until we learn the real sensor uuid (from telemetry)
                            sent["interval_tmpl"] = t
                            log("  (interval: waiting for a sensor uuid to target the wildcard topic)")
                        else:
                            send_interval(t)   # 1.0.x concrete topic: send now
                    # OTA: when the device subscribes to its firmware-update-request topic,
                    # push the 23-byte update offer; the device then pulls chunks (below).
                    if ota and not ota.get("done") and t.endswith("/ota/firmware-update-request") and not sent["ota"]:
                        mj, mn, pt = ota["ver"]
                        offer = bytes([mj, mn, pt]) + ota["total"].to_bytes(4, "big") + ota["md5"]
                        publish_raw(t, offer)
                        log(f"  -> OTA offer v{mj}.{mn}.{pt}  {ota['total']} bytes  "
                            f"md5={ota['md5'].hex()}  ({len(ota['data'])//1024} KB to flash)")
                        sent["ota"] = True

            elif ptype == 3:  # PUBLISH
                qos = (flags >> 1) & 3
                i = 0
                tl = int.from_bytes(body[i:i+2], "big"); i += 2
                topic = body[i:i+tl].decode("utf-8", "replace"); i += tl
                pid = None
                if qos > 0:
                    pid = int.from_bytes(body[i:i+2], "big"); i += 2
                if v5:
                    plen, i = rd_varint(body, i); i += plen
                payload = body[i:]
                log(f"PUBLISH  {topic}\n           {fmt_payload(payload)}")
                if qos == 1 and pid is not None:
                    sock.sendall(pkt(4, 0, pid.to_bytes(2, "big")))  # PUBACK

                # ---- deferred interval command: 1.2.x wildcard needs a concrete sensor uuid ----
                if set_interval and not sent["interval"] and sent.get("interval_tmpl"):
                    suid = None
                    if "/sensor/" in topic and "/state" in topic:          # a sensor state topic
                        suid = topic.split("/sensor/", 1)[1].split("/", 1)[0]
                    else:                                                   # or a bridge heartbeat payload
                        try:
                            for dev in (json.loads(payload).get("paired_devices") or []):
                                if dev.get("device") in (None, "meter") and dev.get("uuid"):
                                    suid = dev["uuid"]; break
                        except Exception:
                            pass
                    if suid and suid != "+":
                        send_interval(sent["interval_tmpl"].replace("/sensor/+/", f"/sensor/{suid}/"))

                # ---- OTA: serve firmware chunks the device asks for ----
                if ota and topic.endswith("/ota/firmware-data-request"):
                    try:
                        off = int(json.loads(payload).get("offset", 0))
                    except Exception:
                        off = 0
                    chunk = ota["data"][off:off + ota["chunk"]]
                    mj, mn, pt = ota["ver"]
                    hdr = bytes([mj, mn, pt]) + off.to_bytes(4, "big") + len(chunk).to_bytes(2, "big")
                    resp_topic = topic[:-len("firmware-data-request")] + "firmware-data-response"
                    publish_raw(resp_topic, hdr + chunk)
                    done = off + len(chunk)
                    last = len(chunk) < ota["chunk"]
                    if last:
                        ota["done"] = True   # don't re-offer after the reboot (avoids a reflash loop)
                    log(f"  -> OTA chunk offset={off} len={len(chunk)}  "
                        f"({100 * done // max(1, ota['total'])}% of {ota['total']})"
                        + ("  [LAST -- will not re-offer]" if last else ""))
                    continue
                if ota and topic.endswith("/ota/firmware-update-response"):
                    log("  <- device ACKed the OTA offer; it will now pull chunks")
                    continue

                # ---- fleet-provisioning auto-responses ----
                if topic.endswith("/certificates/create/json") or \
                   topic.endswith("/certificates/create-from-csr/json"):
                    publish(topic + "/accepted", cert_response(pki))
                elif topic.endswith("/provision/json"):
                    thing, resp = provision_response(payload)
                    publish(topic + "/accepted", resp)
                    log(f"  registered thing: {thing}")
                    if push_shadow:
                        publish(f"$aws/things/{thing}/shadow/update/delta",
                                json.dumps({"state": {"hello": "from-local-cloud"}}))
                elif topic.endswith("/shadow/get"):
                    # minimal empty shadow so the device's get succeeds
                    publish(topic + "/accepted", json.dumps({
                        "state": {"desired": {}, "reported": {}},
                        "metadata": {"desired": {}, "reported": {}},
                        "version": 1, "timestamp": int(time.time())}))
                elif topic.endswith("/shadow/update"):
                    try:
                        st = json.loads(payload).get("state", {})
                    except Exception:
                        st = {}
                    now = int(time.time())
                    publish(topic + "/accepted", json.dumps(
                        {"state": st, "version": 1, "timestamp": now}))
                    publish(topic + "/documents", json.dumps(
                        {"previous": None, "current": {"state": st, "version": 1}, "timestamp": now}))

            elif ptype == 12:  # PINGREQ
                sock.sendall(pkt(13, 0))
            elif ptype == 14:  # DISCONNECT
                break
            # PUBACK(4)/others: ignore
    except (ConnectionError, ssl.SSLError, OSError) as e:
        log(f"conn closed ({addr[0]}): {e}")
    finally:
        try:
            sock.close()
        except Exception:
            pass
        log(f"DISCONNECT client_id={cid!r}")


def load_ota_image(path, version, chunk=512):
    """Read an ESP32 app image and prepare the OTA job (version, total len, md5)."""
    data = open(path, "rb").read()
    if not data:
        sys.exit(f"OTA image {path} is empty")
    if data[0] != 0xE9:
        log(f"WARNING: {path} does not start with 0xE9 (ESP32 app magic) -- the device may reject it")
    try:
        parts = [int(x) for x in version.split(".")]
        ver = tuple((parts + [0, 0, 0])[:3])
    except Exception:
        sys.exit(f"bad --ota-version {version!r} (want X.Y.Z)")
    md5 = hashlib.md5(data).digest()
    log(f"OTA image loaded: {path}  {len(data)} bytes  v{ver[0]}.{ver[1]}.{ver[2]}  md5={md5.hex()}")
    return {"ver": ver, "data": data, "total": len(data), "md5": md5, "chunk": chunk, "done": False}


def main():
    ap = argparse.ArgumentParser(description="local MQTTS cloud for OBI device analysis")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8883)
    ap.add_argument("--pki", default=os.path.join(HERE, "pki"))
    ap.add_argument("--push-shadow", action="store_true",
                    help="push a shadow delta to the thing after provisioning")
    ap.add_argument("--set-interval", type=int, metavar="SECONDS",
                    help="change the reader upload interval (default 300): when the device subscribes "
                         "to its upload-interval-change-request topic, push {sensor_upload_interval,session_id}")
    ap.add_argument("--set-session", type=int, default=1,
                    help="session_id echoed in the upload-interval command (default 1)")
    ap.add_argument("--ota-firmware", metavar="FW.bin",
                    help="push this ESP32 app image to the device over MQTT OTA (unsigned; integrity hash only)")
    ap.add_argument("--ota-version", metavar="X.Y.Z", default="9.9.9",
                    help="version advertised in the OTA offer (default 9.9.9, i.e. 'newer'; cosmetic)")
    args = ap.parse_args()
    set_interval = (args.set_interval, args.set_session) if args.set_interval else None
    ota = load_ota_image(args.ota_firmware, args.ota_version) if args.ota_firmware else None

    for n in ("ca.pem", "server.crt", "server.key", "permanent.crt", "permanent.key"):
        if not os.path.exists(os.path.join(args.pki, n)):
            sys.exit(f"missing {n} in {args.pki} -- run gen_certs.py first")
    pki = load_pki(args.pki)

    # sanity: does server.crt actually cover the IP the device will connect to?
    try:
        info = ssl._ssl._test_decode_cert(os.path.join(args.pki, "server.crt"))  # type: ignore
        names = {v for t in info.get("subject", ()) for k, v in t if k == "commonName"}
        names |= {v for typ, v in info.get("subjectAltName", ())}
        adv = args.host if args.host not in ("0.0.0.0", "::") else None
        covered = ", ".join(sorted(names))
        log(f"server.crt covers: {covered}")
        if adv and adv not in names:
            log(f"  !! WARNING: server.crt does NOT list '{adv}'. The device connects by IP and will "
                f"reject the cert (bad certificate). Re-run: python gen_certs.py --host <that-IP>, "
                f"then re-push ble_config.json over BLE.")
    except Exception:
        pass

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(os.path.join(args.pki, "server.crt"), os.path.join(args.pki, "server.key"))
    ctx.verify_mode = ssl.CERT_OPTIONAL          # accept (and log) the device client cert, don't require
    try:
        ctx.load_verify_locations(os.path.join(args.pki, "ca.pem"))
    except Exception:
        pass

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(5)
    log(f"MQTTS server on {args.host}:{args.port}  (pki={args.pki})  -- waiting for the device")
    try:
        while True:
            raw, addr = srv.accept()
            try:
                tls = ctx.wrap_socket(raw, server_side=True)
            except (ssl.SSLError, OSError) as e:
                log(f"TLS handshake failed from {addr[0]}: {e}")
                if "BAD_CERTIFICATE" in str(e) or "certificate" in str(e).lower():
                    log("  hint: the device rejected OUR server cert. Ensure gen_certs.py was run "
                        "with --host = the IP the device connects to, AND re-push ble_config.json "
                        "over BLE so the device has the matching caPem.")
                try:
                    raw.close()
                except Exception:
                    pass
                continue
            threading.Thread(target=handle, args=(tls, addr, pki, args.push_shadow, set_interval, ota), daemon=True).start()
    except KeyboardInterrupt:
        log("shutting down")
    finally:
        srv.close()


if __name__ == "__main__":
    main()
