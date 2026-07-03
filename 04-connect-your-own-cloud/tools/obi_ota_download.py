#!/usr/bin/env python3
"""
obi_ota_download.py -- pull an OBI energy-tracking bridge firmware image straight from the
cloud by replaying the device's OTA client, and save it as a binary file.

How the OTA works (reverse-engineered live via obi_local_cloud/mitm_proxy.py):
  1. bridge connects (mutual TLS) and publishes its heartbeat/state to
       $aws/rules/EnergyTrackingBridge[Heartbeat]/<uuid>/state   (firmware_version = current)
  2. if a newer firmware exists the cloud pushes an OTA offer to
       cmd/energy-tracking/bridge/<uuid>/ota/firmware-update-request
     payload = 23 bytes: [ver_major][ver_minor][ver_patch][total_len u32 BE][md5 16 bytes]
     (e.g. 01 00 02 | 00173D74 | <md5>  ->  v1.0.2, 1,522,548-byte image; confirmed on a live
      device and byte-for-byte against a real cloud capture -- see cloud-api.md#ota)
  3. device echoes that 23-byte payload back to .../ota/firmware-update-response   (ack)
  4. device pulls the image in 512-byte chunks:
       publish dt/energy-tracking/bridge/<uuid>/ota/firmware-data-request
               {"uuid":..,"firmware_version":"<target>","offset":N}
       receive dt/.../ota/firmware-data-response = 9-byte header + up to 512 firmware bytes
       header = [ver3][offset u32 BE][len u16 BE]; data length = header[7:9]; image starts 0xE9
  5. last chunk is < 512 bytes -> done.

This tool is the DOWNLOAD side (get a stock image from the vendor). To PUSH an image to a device
on your OWN cloud, use mqtts_server.py --ota-firmware (see 04 README / cloud-api.md#ota).

You need a cloud client cert for the bridge's thing (= its UUID) -- the AWS-IoT fleet-provisioning
credentials the app normally pushes (POST /device-provisionings -> {certificatePem, privateKey,
caPem, clusterEndpointUri}; see 03-reverse-engineering/cloud-api.md). Put them in a dir as
ca.pem / client.crt / client.key and pass --from-dir. The backend only offers the update if that
UUID is a bridge your account owns.

Usage:
  python obi_ota_download.py --from-dir certs_bridge --uuid <uuid> --out firmware.bin
  python obi_ota_download.py --from-dir certs_bridge --uuid <uuid> --report-fw 1.0.1 --out fw.bin

Needs: pip install paho-mqtt
"""
from __future__ import annotations
import argparse, json, os, ssl, sys, threading, time

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("needs paho-mqtt -- pip install paho-mqtt")

# your device's AWS-IoT endpoint = clusterEndpointUri from POST /device-provisionings
# (host part of the mqtts:// URL). Pass it with --endpoint.
DEF_ENDPOINT = "<your-device-cloud-endpoint>"
CHUNK = 512


def build_client(ca, cert, key, client_id):
    try:
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id=client_id)
    except (AttributeError, TypeError):
        c = mqtt.Client(client_id=client_id)
    ctx = ssl.create_default_context(cafile=ca)
    ctx.load_cert_chain(cert, key)
    c.tls_set_context(ctx)
    return c


def bridge_state(uuid, fw, hw="6.0.0"):
    return json.dumps({"uuid": uuid, "hardware_version": hw, "firmware_version": fw,
                       "ota": None, "paired_sensor": [{"sensor": None}]})


def download_image(c, topic_req, uuid, target, out_path, chunk_ev, state, chunk_timeout, max_bytes):
    """Pull firmware `target` chunk-by-chunk into out_path; returns bytes written."""
    out = open(out_path, "wb")
    offset = 0
    t0 = time.time()
    try:
        while offset < max_bytes:
            got = None
            for _ in range(4):
                chunk_ev.clear(); state["resp"] = None
                c.publish(topic_req, json.dumps(
                    {"uuid": uuid, "firmware_version": target, "offset": offset}), qos=1)
                if chunk_ev.wait(chunk_timeout):
                    got = state["resp"]; break
            if got is None:
                print(f"[!] {target}: no response at offset {offset} -- aborting"); break
            dlen = int.from_bytes(got[7:9], "big") if len(got) >= 9 else 0
            data = got[9:9 + dlen] if dlen else got[9:]
            out.write(data); offset += len(data)
            if offset % (256 * 1024) < CHUNK:
                print(f"      {target}: {offset // 1024} KB")
            if len(data) < CHUNK:
                break
    finally:
        out.close()
    print(f"[+] {target}: {offset} bytes -> {out_path} ({time.time()-t0:.0f}s)")
    return offset


def main():
    ap = argparse.ArgumentParser(description="Download an OBI bridge firmware image from the cloud.",
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--from-dir", help="dir with ca.pem/client.crt/client.key[/thing.json] (cloud cert)")
    ap.add_argument("--ca"); ap.add_argument("--cert"); ap.add_argument("--key")
    ap.add_argument("--uuid", help="bridge UUID (default: thingName from thing.json)")
    ap.add_argument("--endpoint", default=DEF_ENDPOINT)
    ap.add_argument("--port", type=int, default=8883)
    ap.add_argument("--out", default="firmware.bin", help="output file")
    ap.add_argument("--report-fw", default="1.0.1", help="firmware_version to advertise (older -> triggers OTA)")
    ap.add_argument("--hw", default="6.0.0")
    ap.add_argument("--target-version", help="skip waiting for the offer and pull this version directly")
    ap.add_argument("--probe", help="comma list of versions to probe (offset 0 only), then exit "
                                    "e.g. 1.0.0,1.0.1,1.0.2,1.0.3")
    ap.add_argument("--probe-majors", type=int, metavar="MAX",
                    help="probe 0.0.0..MAX.0.0 (major versions), then exit")
    ap.add_argument("--probe-wait", type=float, default=3.0, help="seconds to wait per probed version")
    ap.add_argument("--versions", help="comma list of versions to download to firmware_<ver>.bin each")
    ap.add_argument("--out-dir", help="directory for --versions downloads (default: current dir)")
    ap.add_argument("--payload-uuid", help="uuid to put in the request JSON (default: the thing uuid; "
                                           "set to a reader/sensor uuid to fetch its firmware)")
    ap.add_argument("--trigger-timeout", type=float, default=30.0, help="s to wait for the OTA offer")
    ap.add_argument("--chunk-timeout", type=float, default=15.0, help="s to wait for each chunk")
    ap.add_argument("--max-bytes", type=int, default=8 * 1024 * 1024, help="safety cap")
    args = ap.parse_args()

    d = args.from_dir
    ca = args.ca or (os.path.join(d, "ca.pem") if d else None)
    cert = args.cert or (os.path.join(d, "client.crt") if d else None)
    key = args.key or (os.path.join(d, "client.key") if d else None)
    if not (ca and cert and key and all(os.path.exists(p) for p in (ca, cert, key))):
        sys.exit("need --ca/--cert/--key or --from-dir with those files")
    uuid = args.uuid
    if not uuid and d and os.path.exists(os.path.join(d, "thing.json")):
        uuid = json.load(open(os.path.join(d, "thing.json"))).get("thingName")
    if not uuid:
        sys.exit("no --uuid (and no thing.json)")
    puuid = args.payload_uuid or uuid   # uuid placed in the request JSON body

    ota = f"energy-tracking/bridge/{uuid}/ota"
    T_UPD = f"cmd/{ota}/firmware-update-request"
    T_UPD_RESP = f"cmd/{ota}/firmware-update-response"
    T_DATA_REQ = f"dt/{ota}/firmware-data-request"
    T_DATA_RESP = f"dt/{ota}/firmware-data-response"
    T_STATE = f"$aws/rules/EnergyTrackingBridge/{uuid}/state"
    T_HB = f"$aws/rules/EnergyTrackingBridgeHeartbeat/{uuid}/state"

    state = {"update": None, "resp": None}
    offer = threading.Event()
    chunk = threading.Event()

    def on_connect(c, u, f, rc, *a):
        print(f"[+] connected to {args.endpoint} as {uuid}")
        c.subscribe(T_UPD, 1)
        c.subscribe(T_DATA_RESP, 1)

    def on_message(c, u, msg):
        if msg.topic == T_UPD:
            state["update"] = bytes(msg.payload)
            offer.set()
        elif msg.topic == T_DATA_RESP:
            state["resp"] = bytes(msg.payload)
            chunk.set()

    def on_disconnect(c, u, rc, *a):
        code = getattr(rc, "value", rc)
        if code != 0 and not state.get("closing"):
            print(f"[!] disconnected (rc={rc}) -- cert policy or client-id conflict?")

    c = build_client(ca, cert, key, uuid)
    c.on_connect = on_connect
    c.on_message = on_message
    c.on_disconnect = on_disconnect
    c.connect(args.endpoint, args.port, keepalive=30)
    c.loop_start()
    time.sleep(1.5)

    # optional: probe which firmware versions the data endpoint will serve (offset 0 only)
    probe_list = None
    if args.probe_majors is not None:
        probe_list = [f"{i}.0.0" for i in range(0, args.probe_majors + 1)]
    elif args.probe:
        probe_list = [v.strip() for v in args.probe.split(",") if v.strip()]
    if probe_list:
        print(f"[i] probing {len(probe_list)} firmware versions (request offset 0)...")
        wait = args.probe_wait
        hits = []
        for ver in probe_list:
            chunk.clear(); state["resp"] = None
            c.publish(T_DATA_REQ, json.dumps(
                {"uuid": puuid, "firmware_version": ver, "offset": 0}), qos=1)
            if chunk.wait(wait):
                r = state["resp"]
                dlen = int.from_bytes(r[7:9], "big") if len(r) >= 9 else 0
                data = r[9:9 + dlen] if dlen else r[9:]
                tag = "EXISTS (ESP32 image)" if data[:1] == b"\xe9" else "responded"
                print(f"    {ver:8s}: {tag}  chunk={len(data)}B first={data[:4].hex()}")
                hits.append(ver)
        print(f"[i] {len(hits)} version(s) exist: {', '.join(hits) if hits else '(none)'}")
        state["closing"] = True
        c.loop_stop()
        try:
            c.disconnect()
        except Exception:
            pass
        return

    # batch: download several versions on one connection
    if args.versions:
        vers = [v.strip() for v in args.versions.split(",") if v.strip()]
        outdir = args.out_dir or "."
        os.makedirs(outdir, exist_ok=True)
        print(f"[i] batch download of {len(vers)} version(s) into {outdir}/")
        for ver in vers:
            download_image(c, T_DATA_REQ, puuid, ver, os.path.join(outdir, f"firmware_{ver}.bin"),
                           chunk, state, args.chunk_timeout, args.max_bytes)
        state["closing"] = True
        c.loop_stop()
        try:
            c.disconnect()
        except Exception:
            pass
        return

    # 1) advertise an older version to trigger the OTA offer
    hb = bridge_state(uuid, args.report_fw, args.hw)
    c.publish(T_HB, hb, qos=1)
    c.publish(T_STATE, hb, qos=1)
    print(f"[~] published heartbeat (firmware_version={args.report_fw}); waiting for OTA offer...")

    target = args.target_version
    if offer.wait(args.trigger_timeout):
        upd = state["update"]
        target = f"{upd[0]}.{upd[1]}.{upd[2]}"
        total = int.from_bytes(upd[3:7], "big") if len(upd) >= 7 else 0
        md5 = upd[7:23].hex() if len(upd) >= 23 else ""
        print(f"[+] OTA offer: version {target}  total={total} bytes  md5={md5}")
        c.publish(T_UPD_RESP, upd, qos=1)   # ack
        print("[~] acked update-request")
    elif target:
        print(f"[i] no offer within {args.trigger_timeout:.0f}s; pulling --target-version {target} anyway")
    else:
        state["closing"] = True
        c.loop_stop(); c.disconnect()
        sys.exit("no OTA offer received (bridge not owned / already up to date?). "
                 "Retry, or pass --target-version to force.")

    # 2) pull the image in 512-byte chunks
    out = open(args.out, "wb")
    offset = 0
    t0 = time.time()
    try:
        while offset < args.max_bytes:
            got = None
            for attempt in range(4):
                chunk.clear()
                state["resp"] = None
                c.publish(T_DATA_REQ, json.dumps(
                    {"uuid": puuid, "firmware_version": target, "offset": offset}), qos=1)
                if chunk.wait(args.chunk_timeout):
                    got = state["resp"]
                    break
                print(f"    [retry offset {offset}, attempt {attempt+1}]")
            if got is None:
                print(f"[!] no response for offset {offset} -- aborting"); break
            dlen = int.from_bytes(got[7:9], "big") if len(got) >= 9 else 0
            data = got[9:9 + dlen] if dlen else got[9:]
            out.write(data)
            offset += len(data)
            if offset % (64 * 1024) < CHUNK:
                print(f"    {offset:>8} bytes ({offset/1024:.0f} KB)")
            if len(data) < CHUNK:
                print(f"[+] final chunk ({len(data)} bytes) at offset {offset}")
                break
    finally:
        out.close()
    dt = time.time() - t0
    print(f"[+] wrote {offset} bytes to {args.out} in {dt:.1f}s")
    if os.path.getsize(args.out):
        head = open(args.out, "rb").read(1)
        magic = "ESP32 image magic OK" if head == b"\xe9" else "not 0xE9"
        print(f"    first byte 0x{head.hex()} ({magic})")

    state["closing"] = True
    c.loop_stop()
    try:
        c.disconnect()
    except Exception:
        pass


if __name__ == "__main__":
    main()
