#!/usr/bin/env python3
"""
ble_provision.py -- BLE side: find an OBI-XXXXXX gateway and set it up in ONE session.

Why one session: the gateway's BLE ("BleSwitch") is only active during a setup window and
goes away once it is operational/cloud-connected. Sensor pairing is BLE-only (there is NO
MQTT/cloud command to add a reader), so it must be done here, together with (or before) the
cloud config. (You can re-open BLE later at any time by holding the gateway's button for ~5 s,
which re-activates it for general config and adding sensors -- then just run this tool again.)
This tool can, in a single connect:
  1. scan for a device advertising OBI-XXXXXX (service ABF0), connect, enable notifications,
  2. StatusRequest,
  3. optional UnbindRequest (clear an existing operational cert),
  4. optional --pair-sensor: SensorScan (poll ~90s) -> shows a MENU of the readers it
     found so you PICK which one to SensorBind (or --sensor-uuid / --first for scripts),
  5. SetTMPCertificateRequest = pki/ble_config.json (CA + claim cert + mqtts:// url) -- pushed
     BEFORE Wi-Fi: this big multi-fragment message would otherwise collide with the Wi-Fi connect
     and overload the gateway's BLE RX heap (fragments dropped -> "ble parse failed"),
  6. optional WifiSetRequest (join your Wi-Fi) -- LAST, so the device then connects with cert set.

After this the device connects to mqtts_server.py, provisions by claim, gets the permanent
cert back, and reconnects -- watch the server log. A freshly bound reader takes a few minutes
to actually report (it first joins over LoRa: ECDH + first energy frame).

Reuses the TEA/framing codec from 06-tools/obi_ble_codec.py.  Needs: pip install bleak

Reader pairing is ON by default (that is the whole point of the one-session flow); pass
--no-pair-sensor to skip it.

Usage:
  python ble_provision.py --config pki/ble_config.json --key <32hex> --unbind \
      --ssid <wifi> --password <pw>        # unbind, push cloud config, and pick a reader to bind
  python ble_provision.py --config pki/ble_config.json --key <32hex> \
      --sensor-uuid <reader-uuid>          # bind a specific reader (non-interactive)
  python ble_provision.py --key <32hex> --no-pair-sensor --status-only   # just read status
"""
from __future__ import annotations
import argparse, asyncio, json, os, sys

# find obi_ble_codec.py (repo layout: 06-tools/; or a sibling/parent for standalone use)
_here = os.path.dirname(os.path.abspath(__file__))
for _p in (os.path.join(_here, "..", "..", "06-tools"),
           os.path.dirname(_here),
           os.path.dirname(os.path.dirname(_here)), _here):
    sys.path.insert(0, os.path.abspath(_p))
try:
    from obi_ble_codec import encode_request, tea_ecb, parse_frame
except Exception as e:
    sys.exit(f"cannot import obi_ble_codec.py (expected in 06-tools/): {e}")
try:
    from bleak import BleakScanner, BleakClient
except ImportError:
    sys.exit("needs 'bleak' -- pip install bleak")

SVC = "0000abf0-0000-1000-8000-00805f9b34fb"
ABF1 = "0000abf1-0000-1000-8000-00805f9b34fb"   # notify (gateway -> app)
ABF2 = "0000abf2-0000-1000-8000-00805f9b34fb"   # write  (app -> gateway)


class OBILink:
    def __init__(self, client, key, chunk_size=173, frag_delay=0.06):
        self.client = client
        self.key = key
        self.txn = 0
        self.chunk_size = chunk_size
        self.frag_delay = frag_delay   # pause between BLE fragment writes (s)
        self.rxbuf = {}
        self.q = asyncio.Queue()
        self.loop = asyncio.get_event_loop()

    def on_notify(self, _sender, data):
        try:
            fr = parse_frame(tea_ecb(bytes(data), self.key, encrypt=False))
        except Exception as e:
            print(f"[rx] frame decode error: {e}  ({bytes(data).hex()})")
            return
        self.rxbuf.setdefault(fr.number, {})[fr.index] = fr.payload
        if fr.last:
            frags = self.rxbuf.pop(fr.number)
            msg = b"".join(frags[i] for i in sorted(frags))
            self.loop.call_soon_threadsafe(self.q.put_nowait, msg.decode("utf-8", "replace"))

    async def write_only(self, obj):
        """Send a request without waiting for the reply (caller drains self.q).
        Multi-fragment messages (e.g. SetTMPCertificate) must be paced: the gateway drops
        fragments it can't process in time ('bleserver deal data is too slow.throw frame!'),
        which breaks reassembly. frag_delay seconds between writes avoids that."""
        n = self.txn
        self.txn = (self.txn + 1) % 127
        # The fragment index is a single header byte (0..255 -> max 256 fragments). When the ATT
        # MTU never got negotiated and stayed at the BLE default 23, the auto chunk size becomes
        # tiny (mtu-13 = 10B). A big multi-fragment message like SetTMPCertificate then needs more
        # than 256 fragments and encode_request raises "index must be 0..255". Guard against it by
        # bumping the fragment size just enough to fit the whole message in <=256 fragments.
        js = obj if isinstance(obj, str) else json.dumps(obj, separators=(",", ":"))
        nbytes = len(js.encode("utf-8"))
        # Hard protocol ceiling: index is 1 byte (256 fragments) x 173B max payload = 44288B.
        # A message above that simply cannot be framed at any fragment size -- fail loudly.
        MAX_MSG = 256 * 173
        if nbytes > MAX_MSG:
            raise ValueError(f"message is {nbytes}B but the BLE protocol caps a single message at "
                             f"{MAX_MSG}B (256 fragments x 173B). Shrink the payload "
                             f"(e.g. smaller/EC certs) -- it cannot be sent over this link.")
        chunk = self.chunk_size
        need = -(-nbytes // 256)                      # min payload/fragment to fit in 256 frames
        if need > chunk:
            chunk = min(173, need)                    # 173 = MAX_PAYLOAD (codec hard cap)
            print(f"[!] fragment payload {self.chunk_size}B too small for this {nbytes}B message "
                  f"(would need >256 fragments); raising to {chunk}B for this write.")
        try:
            frames = list(encode_request(obj, self.key, n, chunk))
        except ValueError as e:
            # last-resort safety net: retry once at the largest fragment size the codec allows
            print(f"[!] framing failed ({e}); retrying at max fragment size (173B).")
            frames = list(encode_request(obj, self.key, n, 173))
        for i, fr in enumerate(frames):
            await self.client.write_gatt_char(ABF2, fr, response=True)
            if self.frag_delay and i < len(frames) - 1:
                await asyncio.sleep(self.frag_delay)

    async def send(self, obj, timeout=20):
        await self.write_only(obj)
        try:
            return await asyncio.wait_for(self.q.get(), timeout)
        except asyncio.TimeoutError:
            return None


def show(label, resp):
    print(f"\n=== {label} ===")
    if resp is None:
        print("  (no response / timeout)")
        return
    try:
        print(json.dumps(json.loads(resp), indent=2, ensure_ascii=False))
    except Exception:
        print("  " + resp)


def _norm(u):
    return (u or "").replace("-", "").lower()


async def _scan_readers(link, scan_timeout, stop_on=None, stop_after_first=False, grace=3.0):
    """Poll SensorScan for up to scan_timeout s, collecting every reader seen.
    Returns dict {uuid: last_data}.
    - stop_on: return as soon as that specific UUID appears (--sensor-uuid path).
    - stop_after_first: return shortly after the FIRST reader answers instead of waiting the
      whole window; `grace` extra seconds let other readers answering at the same time still
      show up (the menu's rescan looks again). scan_timeout stays the cap if nothing answers."""
    loop = asyncio.get_event_loop()
    end = loop.time() + scan_timeout
    last_req = 0.0
    seen = {}
    first_at = None
    while loop.time() < end:
        now = loop.time()
        if stop_after_first and first_at is not None and now - first_at >= grace:
            break                                    # a reader answered -> stop soon, don't wait 90s
        if now - last_req > 25:                      # (re)trigger the LoRa scan periodically
            await link.write_only({"type": "SensorScanRequest"})
            last_req = now
        try:
            msg = await asyncio.wait_for(link.q.get(), 2)
        except asyncio.TimeoutError:
            continue
        try:
            j = json.loads(msg)
        except Exception:
            continue
        if j.get("type") == "SensorScan" and j.get("data"):
            d = j["data"]; uuid = d.get("uuid")
            if not uuid:
                continue
            first = _norm(uuid) not in {_norm(u) for u in seen}
            seen[uuid] = d
            if first:
                print(f"[+] reader found ({len(seen)}): {uuid}  "
                      f"(rssi={d.get('rssi')}, battery={d.get('battery')})")
                if first_at is None:
                    first_at = loop.time()
            if stop_on is not None and _norm(uuid) == _norm(stop_on):
                return {uuid: d}
    return seen


async def _choose(prompt):
    """Blocking input() without freezing the event loop / BLE notifications."""
    loop = asyncio.get_event_loop()
    return (await loop.run_in_executor(None, input, prompt)).strip()


def _reader_live(d):
    """A bound reader is only 'there' once it has actually been heard over LoRa: its **rssi** and
    **battery** turn non-zero (the reader reported). `last_seen` is a Unix timestamp that is set at
    bind time already, so it is NOT a liveness signal -- ignore it. Returns (is_live, rssi, battery, last_seen)."""
    if not isinstance(d, dict):
        return False, None, None, None
    rssi, batt, seen = d.get("rssi"), d.get("battery"), d.get("last_seen")
    return (bool(rssi) or bool(batt)), rssi, batt, seen


async def _bind(link, uuid, reader_timeout=120.0):
    show("SensorBindRequest", await link.send(
        {"type": "SensorBindRequest", "data": {"uuid": uuid}}, timeout=15))
    if reader_timeout <= 0:
        print(f"[i] bound {uuid}. It reports on its own after the LoRa join (ECDH + first frame).")
        return uuid
    print(f"[i] bound {uuid}. Waiting up to {reader_timeout:.0f}s for it to come online over LoRa "
          "(ECDH + first report) -- polling SensorRequest for real values...")
    loop = asyncio.get_event_loop()
    end = loop.time() + reader_timeout
    while loop.time() < end:
        await asyncio.sleep(10)
        try:
            st = await link.send({"type": "SensorRequest"}, timeout=8)
        except Exception:
            break
        try:
            d = json.loads(st).get("data") if st else None
        except Exception:
            d = None
        live, rssi, batt, seen = _reader_live(d)
        if live:
            print(f"[+] reader is live: rssi={rssi}  battery={batt}  last_seen={seen}")
            show("SensorRequest (reader live)", st)
            return uuid
        print(f"    ...reader not reporting yet (rssi={rssi}, battery={batt}, last_seen={seen})")
    print("[i] reader bound but not live yet -- the LoRa join can take a few minutes. It will start "
          "reporting on its own; verify later with SensorRequest or in the broker telemetry.")
    return uuid


async def pair_sensor(link, want=None, scan_timeout=90.0, auto_first=False, reader_timeout=120.0):
    """SensorScan for nearby readers, then SensorBind one, then wait until it actually reports.
      - want set (--sensor-uuid): bind exactly that UUID (non-interactive).
      - auto_first (--first): bind the first reader seen (non-interactive).
      - otherwise: collect all readers during the window and let the user PICK one.
    After binding, polls SensorRequest until the reader is live (reader_timeout).
    Returns the bound UUID (or None)."""
    print(f"\n[i] pairing a reader: scanning (stops as soon as a reader answers, up to {scan_timeout:.0f}s)"
          + (f" for {want}" if want else "") + " ...")
    print("    (make sure the reader is powered and at the meter; a fresh reader can take a bit to answer)")

    # non-interactive: specific UUID
    if want is not None:
        seen = await _scan_readers(link, scan_timeout, stop_on=want)
        match = next((u for u in seen if _norm(u) == _norm(want)), None)
        if not match:
            print(f"[!] reader {want} not seen in time -- powered and in range?")
            return None
        return await _bind(link, match, reader_timeout)

    seen = await _scan_readers(link, scan_timeout, stop_after_first=True)
    if not seen:
        print("[!] no reader found in time -- is it powered and in range?")
        return None

    readers = list(seen.items())
    # non-interactive: first one wins
    if auto_first:
        uuid = readers[0][0]
        print(f"[i] --first: binding {uuid}")
        return await _bind(link, uuid, reader_timeout)

    # interactive selection menu
    while True:
        print("\n  Readers found:")
        for i, (uuid, d) in enumerate(readers, 1):
            print(f"    [{i}] {uuid}   rssi={d.get('rssi')}  battery={d.get('battery')}")
        ans = await _choose("\n  Pick a reader to bind [1-%d], 'r'=rescan, 'q'=skip: " % len(readers))
        if ans.lower() in ("q", ""):
            print("[i] skipping reader pairing.")
            return None
        if ans.lower() == "r":
            more = await _scan_readers(link, scan_timeout, stop_after_first=True)
            for uuid, d in more.items():
                if not any(_norm(uuid) == _norm(u) for u, _ in readers):
                    readers.append((uuid, d))
            continue
        if ans.isdigit() and 1 <= int(ans) <= len(readers):
            return await _bind(link, readers[int(ans) - 1][0], reader_timeout)
        print("[!] invalid choice.")


async def wait_for_connection(link, cfg, timeout):
    """After Wi-Fi is set, poll Status until the device reports it joined Wi-Fi. That reply only
    becomes meaningful once the connect finishes -- which is also when the device provisions
    against your broker. Returns True once connected (or BLE goes operational-quiet)."""
    target = cfg.get("url") if cfg else "your broker"
    print(f"\n[i] waiting up to {timeout:.0f}s for the device to join Wi-Fi (polling Status)...")
    loop = asyncio.get_event_loop()
    end = loop.time() + timeout
    while loop.time() < end:
        await asyncio.sleep(3)
        try:
            st = await link.send({"type": "StatusRequest"}, timeout=6)
        except Exception:
            st = None                                # BLE link dropped (device went operational)
        if st is None:
            # no BLE reply: the gateway most likely went operational and closed BleSwitch
            print("[i] BLE went quiet -- the device likely went operational (BleSwitch off after "
                  "provisioning). Check the broker log for the CONNECT + telemetry.")
            return True
        try:
            d = json.loads(st).get("data", {})
        except Exception:
            d = {}
        cw = d.get("connected_wifi")
        if cw:
            print(f"[+] device joined Wi-Fi (connected_wifi={cw!r}) -> now provisioning against "
                  f"{target}. Watch the broker log for CONNECT -> register thing -> telemetry.")
            show("StatusRequest (connected)", st)
            return True
        print(f"    ...not connected yet (wifi_set={d.get('wifi_set')}, connected_wifi={cw})")
    print("[!] no Wi-Fi connection reported within the timeout. It may still connect -- check the "
          "broker log; otherwise re-check SSID/password and that the server IP matches the cert.")
    return False


async def run(args):
    key = bytes.fromhex(args.key.replace(" ", ""))
    if len(key) != 16:
        sys.exit("--key must be 16 bytes (32 hex chars)")
    cfg = None
    if args.config:
        cfg = json.load(open(args.config, encoding="utf-8"))
        if "data" in cfg and isinstance(cfg["data"], dict):
            cfg = cfg["data"]

    addr = await find_device(args)
    print(f"[i] connecting {addr}...")
    async with BleakClient(addr) as client:
        link = OBILink(client, key, frag_delay=args.frag_delay)
        await client.start_notify(ABF1, link.on_notify)
        mtu = getattr(client, "mtu_size", 0) or 0
        if args.frag:
            link.chunk_size = args.frag
        elif mtu:
            link.chunk_size = max(8, min(173, mtu - 13))
        print(f"[+] connected, notifications on ABF1  (MTU={mtu or '?'}, fragment payload={link.chunk_size}B)")

        st = await link.send({"type": "StatusRequest"})
        show("StatusRequest", st)
        if args.status_only:
            return
        persistent = False
        try:
            persistent = bool(json.loads(st).get("data", {}).get("persistent_cert_set"))
        except Exception:
            pass
        if persistent and not args.unbind and cfg:
            print("\n[!] persistent_cert_set = TRUE: the device already has an operational cert and will")
            print("    ignore this TMP cert. Re-run with --unbind to force re-provisioning locally.")

        if args.unbind:
            show("UnbindRequest", await link.send({"type": "UnbindRequest"}, timeout=args.cert_timeout))
            show("StatusRequest (after unbind)", await link.send({"type": "StatusRequest"}))

        # Push the (big, multi-fragment) certificate FIRST -- while the device is calmest, right
        # after unbind and BEFORE any LoRa scan / reader-join / Wi-Fi load (those overload the BLE
        # RX heap and drop fragments). The device does NOT send a BLE reply to SetTMPCertificate, so
        # FIRE-AND-FORGET it -- don't block waiting for a response. The real confirmation is the
        # provisioning once Wi-Fi comes up (wait_for_connection). Order: cert -> pair -> Wi-Fi.
        if cfg:
            await asyncio.sleep(0.5)                 # settle after unbind
            print("[i] sending SetTMPCertificate (fire-and-forget, no reply expected)...")
            await link.write_only({"type": "SetTMPCertificateRequest", "data": cfg})
            await asyncio.sleep(1.5)                 # let the device reassemble + store it
            while not link.q.empty():                # drop any late/stray reply
                link.q.get_nowait()
            print("[i] cert sent")

        # pair the reader in the SAME session (BLE-only; must happen during setup)
        if args.pair_sensor:
            await pair_sensor(link, args.sensor_uuid, args.scan_timeout, args.first, args.reader_timeout)
        if args.ssid:
            show("WifiSetRequest", await link.send(
                {"type": "WifiSetRequest", "data": {"ssid": args.ssid, "password": args.password}}))
            print(f"\n[i] Wi-Fi set -> device should now connect to {cfg.get('url') if cfg else 'your broker'}")
            # The confirmation we actually care about only appears AFTER the Wi-Fi join completes,
            # so poll Status until the device reports it connected (rather than blindly idling).
            await wait_for_connection(link, cfg, args.connect_timeout)
        else:
            print("[i] idling 20s for further notifications (Ctrl+C to stop)...")
            try:
                while True:
                    show("async notification", await asyncio.wait_for(link.q.get(), 20))
            except (asyncio.TimeoutError, KeyboardInterrupt):
                pass


async def find_device(args):
    if args.address:
        return args.address
    print(f"[i] scanning {args.timeout}s for {args.name}...")
    found = {}
    def cb(d, adv):
        nm = adv.local_name or d.name
        if nm and nm.startswith(args.name):
            found[d.address] = (d, nm)
    scanner = BleakScanner(detection_callback=cb, service_uuids=[SVC])
    await scanner.start(); await asyncio.sleep(args.timeout); await scanner.stop()
    if not found:
        for d in await BleakScanner.discover(timeout=args.timeout):
            if d.name and d.name.startswith(args.name):
                found[d.address] = (d, d.name)
    if not found:
        sys.exit(f"no {args.name}* device found -- is it advertising / in setup mode?")
    addr, (dev, nm) = next(iter(found.items()))
    print(f"[+] found {nm}  [{addr}]")
    return addr


def main():
    ap = argparse.ArgumentParser(description="BLE setup for an OBI gateway: unbind, pair a reader, push local-cloud config")
    ap.add_argument("--config", help="pki/ble_config.json from gen_certs.py (omit for pair/status only)")
    ap.add_argument("--key", required=True, help="device TEA key (32 hex) -- see fetch_tea_key.py or UART cmd 49")
    ap.add_argument("--ssid", help="Wi-Fi SSID to set (so it reaches the server)")
    ap.add_argument("--password", default="",
                     help="Wi-Fi password -- stock gateway firmware 1.0.1 only handles up to 32 bytes "
                          "correctly; a longer one is silently truncated/rejected and the device fails "
                          "to join")
    ap.add_argument("--unbind", action="store_true", help="send UnbindRequest first (clear existing cert)")
    ap.add_argument("--pair-sensor", dest="pair_sensor", action="store_true", default=True,
                    help="scan for readers and bind one -- ON by default (BLE-only!)")
    ap.add_argument("--no-pair-sensor", dest="pair_sensor", action="store_false",
                    help="skip reader pairing (e.g. --status-only or reader already bound)")
    ap.add_argument("--sensor-uuid", help="bind this specific reader UUID (non-interactive)")
    ap.add_argument("--first", action="store_true", help="bind the first reader seen (non-interactive; else you pick from a menu)")
    ap.add_argument("--scan-timeout", type=float, default=90.0, help="seconds to scan for readers")
    ap.add_argument("--reader-timeout", type=float, default=120.0, help="after bind, poll SensorRequest up to this many seconds for the reader to come online (report real rssi/battery/last_seen). 0 = don't wait")
    ap.add_argument("--address", help="BLE address (skip scan)")
    ap.add_argument("--name", default="OBI-", help="advertising name prefix (default OBI-)")
    ap.add_argument("--timeout", type=float, default=8.0, help="device scan timeout seconds")
    ap.add_argument("--cert-timeout", type=float, default=30.0, help="wait for SetTMPCertificate/Unbind reply")
    ap.add_argument("--connect-timeout", type=float, default=45.0, help="after Wi-Fi set, poll Status up to this many seconds for the device to report it joined Wi-Fi")
    ap.add_argument("--status-only", action="store_true", help="only query Status, then exit")
    ap.add_argument("--frag", type=int, default=0, help="fragment payload bytes per write (0=auto; try 100 if cert push times out)")
    ap.add_argument("--frag-delay", type=float, default=0.06, help="seconds between BLE fragment writes (default 0.06; the gateway drops fragments sent too fast -> 'ble parse failed'. raise to 0.1 if SetTMPCertificate still fails)")
    args = ap.parse_args()
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
