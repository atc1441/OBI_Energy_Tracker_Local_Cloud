#!/usr/bin/env python3
"""
mitm_proxy.py -- transparent MITM between the OBI device and the real OBI/AWS-IoT cloud.

Device side  : we are the MQTTS server the device connects to (certs from BLE push).
               The device provisions locally (faked) and becomes operational against us.
Cloud side   : we hold a real-cloud MQTT connection AS the device's thing, using the AWS-IoT
               client cert for the device UUID. Because the account owns that bridge (enrolled
               once via the real app), forwarding the device's heartbeat makes it show up ONLINE
               in the app. Get that cert from the vendor's fleet-provisioning credentials (see
               Prereqs) -- there is intentionally no client here that talks TO the vendor cloud.
Bridging     : device uplink  ($aws/rules/.../state, and anything else it publishes) -> real cloud
               cloud downlink  (OTA cmd, firmware-data, shadow, jobs)                -> device
               Everything is logged; this is the MITM tap.

So: enroll the device once via the real app (creates the owned thing), reset it, then run this
proxy + push the local-cloud config over BLE (ble_provision.py). The device re-registers through
us, we relay to the real cloud, and it appears/behaves as a real bridge -- while we sit in between.

Prereqs:
  python gen_certs.py --host <LAN-IP>          # device-side PKI + ble_config.json
  # Cloud-side cert (certs_bridge/): fetch the vendor's AWS-IoT fleet-provisioning credentials for
  # a bridge YOUR account owns, then save them into certs_bridge/ as:
  #   caPem          -> certs_bridge/ca.pem
  #   certificatePem -> certs_bridge/client.crt
  #   privateKey     -> certs_bridge/client.key
  #   clusterEndpointUri -> host part = --cloud-endpoint (or edit CLOUD_ENDPOINT below)
  # Get them with an authenticated app request:  POST /device-provisionings  body {"bridgeId":"<uuid>"}
  #   -> {caPem, certificatePem, privateKey, clusterEndpointUri, provisioningTemplateName}
  # (see ../../03-reverse-engineering/cloud-api.md). No script that provisions against the vendor
  # cloud is shipped here on purpose -- do this manually against your own account only.

Usage:
  python mitm_proxy.py --host <LAN-IP> --uuid <device-uuid> --cloud-dir certs_bridge
Needs: pip install paho-mqtt
"""
from __future__ import annotations
import argparse, json, os, socket, ssl, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from mqtts_server import (str16, pkt, recv_packet, rd_varint, load_pki,
                          cert_response, provision_response, fmt_payload, log)
try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("needs paho-mqtt -- pip install paho-mqtt")

CLOUD_ENDPOINT = "<your-device-cloud-endpoint>"


def downlink_topics(uuid):
    return [
        f"cmd/energy-tracking/bridge/{uuid}/ota/firmware-update-request",
        f"dt/energy-tracking/bridge/{uuid}/ota/firmware-data-response",
        f"$aws/things/{uuid}/shadow/#",
        f"$aws/things/{uuid}/jobs/#",
    ]


class Proxy:
    def __init__(self, uuid, pki, cloud, fake_provision=True):
        self.uuid = uuid
        self.pki = pki
        self.cloud = cloud
        self.fake_provision = fake_provision
        self.dev_sock = None
        self.dev_v5 = False
        self.lock = threading.Lock()

    # ---- cloud -> device ----
    def on_cloud_message(self, cl, u, msg):
        log(f"CLOUD->DEV  {msg.topic}\n            {fmt_payload(msg.payload)}")
        self._to_device(msg.topic, msg.payload)

    def _to_device(self, topic, payload):
        with self.lock:
            s = self.dev_sock
            if not s:
                return
            body = str16(topic) + (b"\x00" if self.dev_v5 else b"") + payload
            try:
                s.sendall(pkt(3, 0, body))
            except Exception as e:
                log(f"  (device write failed: {e})")

    # ---- device -> cloud ----
    def forward_to_cloud(self, topic, payload):
        try:
            self.cloud.publish(topic, payload, qos=0)
            log(f"DEV->CLOUD  {topic}")
        except Exception as e:
            log(f"  (cloud publish failed: {e})")


def handle_device(sock, addr, proxy):
    v5 = False
    cid = "?"
    peer_cn = "?"
    try:
        cert = sock.getpeercert()
        if cert:
            for tup in cert.get("subject", ()):
                for k, v in tup:
                    if k == "commonName":
                        peer_cn = v
    except Exception:
        pass
    with proxy.lock:
        proxy.dev_sock = sock
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
                proxy.dev_v5 = v5
                i += 1 + 2
                if v5:
                    plen, i = rd_varint(body, i); i += plen
                clen = int.from_bytes(body[i:i+2], "big"); i += 2
                cid = body[i:i+clen].decode("utf-8", "replace")
                sock.sendall(pkt(2, 0, b"\x00\x00\x00" if v5 else b"\x00\x00"))
                log(f"DEV CONNECT  client_id={cid!r}  cert_cn={peer_cn}  mqtt={'5.0' if v5 else '3.1.1'}")

            elif ptype == 8:  # SUBSCRIBE
                pid = int.from_bytes(body[0:2], "big"); i = 2
                if v5:
                    plen, i = rd_varint(body, i); i += plen
                topics = []
                while i < len(body):
                    tl = int.from_bytes(body[i:i+2], "big"); i += 2
                    topics.append(body[i:i+tl].decode("utf-8", "replace")); i += tl
                    i += 1
                sock.sendall(pkt(9, 0, pid.to_bytes(2, "big") + (b"\x00" if v5 else b"") + b"\x00" * len(topics)))
                for t in topics:
                    log(f"DEV SUBSCRIBE  {t}")

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
                log(f"DEV PUBLISH  {topic}\n            {fmt_payload(payload)}")
                if qos == 1 and pid is not None:
                    sock.sendall(pkt(4, 0, pid.to_bytes(2, "big")))

                prov = (topic.endswith("/certificates/create/json") or
                        topic.endswith("/certificates/create-from-csr/json") or
                        topic.endswith("/provision/json"))
                if prov and proxy.fake_provision:
                    def reply(t, data):
                        b = str16(t) + (b"\x00" if v5 else b"") + data.encode()
                        sock.sendall(pkt(3, 0, b)); log(f"  -> local provisioning reply {t}")
                    if topic.endswith("/provision/json"):
                        thing, resp = provision_response(payload)
                        reply(topic + "/accepted", resp)
                        log(f"  device provisioned locally as thing: {thing}")
                    else:
                        reply(topic + "/accepted", cert_response(proxy.pki))
                else:
                    # operational traffic -> relay to the real cloud
                    proxy.forward_to_cloud(topic, payload)

            elif ptype == 12:  # PINGREQ
                sock.sendall(pkt(13, 0))
            elif ptype == 14:  # DISCONNECT
                break
    except (ConnectionError, ssl.SSLError, OSError) as e:
        log(f"device conn closed: {e}")
    finally:
        with proxy.lock:
            if proxy.dev_sock is sock:
                proxy.dev_sock = None
        try:
            sock.close()
        except Exception:
            pass
        log(f"DEV DISCONNECT client_id={cid!r}")


def make_cloud(ca, cert, key, client_id, alpn=None):
    try:
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id=client_id)
    except (AttributeError, TypeError):
        c = mqtt.Client(client_id=client_id)
    ctx = ssl.create_default_context(cafile=ca)
    ctx.load_cert_chain(cert, key)
    if alpn:
        ctx.set_alpn_protocols([alpn])
    c.tls_set_context(ctx)
    return c


def main():
    ap = argparse.ArgumentParser(description="MITM proxy: OBI device <-> real cloud")
    ap.add_argument("--host", default="0.0.0.0", help="bind address (device side)")
    ap.add_argument("--port", type=int, default=8883)
    ap.add_argument("--pki", default=os.path.join(HERE, "pki"))
    ap.add_argument("--uuid", required=True, help="device UUID (used for topics + cloud thing)")
    ap.add_argument("--cloud-dir", default=os.path.join(HERE, "certs_bridge"),
                    help="dir with ca.pem/client.crt/client.key for the REAL cloud (provisioned for --uuid)")
    ap.add_argument("--cloud-endpoint", default=CLOUD_ENDPOINT)
    ap.add_argument("--cloud-port", type=int, default=8883)
    ap.add_argument("--no-cloud", action="store_true", help="don't connect to the real cloud (device side only)")
    args = ap.parse_args()

    for n in ("ca.pem", "server.crt", "server.key", "permanent.crt", "permanent.key"):
        if not os.path.exists(os.path.join(args.pki, n)):
            sys.exit(f"missing {n} in {args.pki} -- run gen_certs.py first")
    pki = load_pki(args.pki)

    cloud = None
    if not args.no_cloud:
        for n in ("ca.pem", "client.crt", "client.key"):
            if not os.path.exists(os.path.join(args.cloud_dir, n)):
                sys.exit(f"missing {n} in {args.cloud_dir} -- put the vendor fleet-provisioning cert for "
                         f"{args.uuid} there first (caPem->ca.pem, certificatePem->client.crt, "
                         f"privateKey->client.key; POST /device-provisionings body {{\"bridgeId\":\"{args.uuid}\"}}).\n"
                         f"See this file's header / ../../03-reverse-engineering/cloud-api.md. "
                         f"Run with --no-cloud to skip the cloud side.")
        cloud = make_cloud(os.path.join(args.cloud_dir, "ca.pem"),
                           os.path.join(args.cloud_dir, "client.crt"),
                           os.path.join(args.cloud_dir, "client.key"), args.uuid)

    proxy = Proxy(args.uuid, pki, cloud)

    if cloud is not None:
        cloud.on_message = proxy.on_cloud_message
        cloud.on_connect = lambda c, u, f, rc, *a: [
            log(f"CLOUD connected ({args.cloud_endpoint}) as {args.uuid}"),
            [c.subscribe(t, 1) for t in downlink_topics(args.uuid)],
            log("CLOUD subscribed downlink: OTA cmd / firmware-data / shadow / jobs")]
        cloud.on_disconnect = lambda c, u, rc, *a: log(f"CLOUD disconnected (rc={rc})")
        try:
            cloud.connect(args.cloud_endpoint, args.cloud_port, keepalive=30)
        except Exception as e:
            sys.exit(f"cloud connect failed: {e}")
        cloud.loop_start()

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(os.path.join(args.pki, "server.crt"), os.path.join(args.pki, "server.key"))
    ctx.verify_mode = ssl.CERT_OPTIONAL
    try:
        ctx.load_verify_locations(os.path.join(args.pki, "ca.pem"))
    except Exception:
        pass

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(5)
    log(f"MITM device-side MQTTS on {args.host}:{args.port}  uuid={args.uuid}  "
        f"cloud={'off' if args.no_cloud else args.cloud_endpoint}")
    try:
        while True:
            raw, addr = srv.accept()
            try:
                tls = ctx.wrap_socket(raw, server_side=True)
            except (ssl.SSLError, OSError) as e:
                log(f"device TLS handshake failed from {addr[0]}: {e}")
                try:
                    raw.close()
                except Exception:
                    pass
                continue
            threading.Thread(target=handle_device, args=(tls, addr, proxy), daemon=True).start()
    except KeyboardInterrupt:
        log("shutting down")
    finally:
        srv.close()
        if cloud is not None:
            cloud.loop_stop()


if __name__ == "__main__":
    main()
