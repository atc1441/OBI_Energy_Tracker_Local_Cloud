# push.py — build + OTA-push the gateway firmware to a running bridge, ESPHome-"run" style.
#
# The gateway already serves an HTTP self-update endpoint (POST /api/selfupdate, multipart field
# "firmware") that writes the image into the inactive OTA slot and reboots — see gateway_web.cpp
# handleSelfOtaUpload(). This script drives that endpoint so a rebuild lands on the device without
# touching a browser. For the sealed OBI_BOARD_OBI_C3 (locked bootloader, no UART) this is THE flash path.
#
# Two ways to run it:
#   1) PlatformIO target (builds first, then pushes):
#          pio run -e obi_gateway_c3 -t push
#      Bridge address + login come from platformio.ini custom_push_* options. Keep them blank in the
#      committed platformio.ini and put your real values in platformio.local.ini (git-ignored, merged
#      via [platformio] extra_configs). Environment variables OBI_BRIDGE_HOST/USER/PASS override both.
#   2) Standalone (push an already-built .bin):
#          python push.py --host 192.168.x.x --user admin --pass secret \
#                         --firmware .pio/build/obi_gateway_c3/firmware.bin
#
# Auth: if the bridge has no web user configured (open access), login is skipped automatically. If a
# user IS set, we POST /api/login (form user=/pass=) to obtain the obi_sess cookie, then send it with
# the upload. After the reboot we re-login (sessions live in RAM and are wiped by the restart) and read
# /api/status to report the firmware version the device came back on.
#
# Stdlib only, so it adds no dependency to the build environment.

import io
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

DEFAULT_TIMEOUT = 30       # seconds for a single HTTP request
REBOOT_WAIT     = 60       # seconds to wait for the device to come back after flashing


# --------------------------------------------------------------------------- HTTP helpers
def _base(host):
    """Normalise a host / URL into http://host (no trailing slash)."""
    host = host.strip().rstrip("/")
    if not host.startswith(("http://", "https://")):
        host = "http://" + host
    return host


def _login(base, user, password, timeout):
    """POST /api/login and return the obi_sess cookie value, or '' if auth is disabled/unset."""
    if not user:
        return ""                       # no credentials -> assume open bridge, skip login
    body = urllib.parse.urlencode({"user": user, "pass": password}).encode()
    req = urllib.request.Request(base + "/api/login", data=body, method="POST")
    req.add_header("Content-Type", "application/x-www-form-urlencoded")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        set_cookie = r.headers.get("Set-Cookie", "")
    # Set-Cookie: obi_sess=<tok>; Path=/; ...
    for part in set_cookie.split(";"):
        part = part.strip()
        if part.startswith("obi_sess="):
            return part[len("obi_sess="):]
    return ""                           # auth disabled on the device -> {"ok":true}, no cookie


def _upload(base, cookie, firmware, timeout):
    """POST the .bin to /api/selfupdate as multipart/form-data; return the parsed JSON dict."""
    with open(firmware, "rb") as f:
        data = f.read()
    boundary = "----obipush%d" % len(data)
    body = io.BytesIO()
    body.write(("--%s\r\n" % boundary).encode())
    body.write(b'Content-Disposition: form-data; name="firmware"; '
               b'filename="firmware.bin"\r\n')
    body.write(b"Content-Type: application/octet-stream\r\n\r\n")
    body.write(data)
    body.write(("\r\n--%s--\r\n" % boundary).encode())
    payload = body.getvalue()

    req = urllib.request.Request(base + "/api/selfupdate", data=payload, method="POST")
    req.add_header("Content-Type", "multipart/form-data; boundary=%s" % boundary)
    req.add_header("Content-Length", str(len(payload)))
    if cookie:
        req.add_header("Cookie", "obi_sess=" + cookie)
    # The device answers {"ok":true} and then reboots; the response arrives before the restart.
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode() or "{}")


def _status(base, cookie, timeout):
    """GET /api/status and return the parsed dict (raises on failure)."""
    req = urllib.request.Request(base + "/api/status", method="GET")
    if cookie:
        req.add_header("Cookie", "obi_sess=" + cookie)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode() or "{}")


def _fw_version(status):
    fw = status.get("fw") if isinstance(status, dict) else None
    if isinstance(fw, dict):
        v, b = fw.get("version"), fw.get("build")
        return "%s (build %s)" % (v, b) if b is not None else str(v)
    return "?"


# --------------------------------------------------------------------------- orchestration
def push(host, user, password, firmware, timeout=DEFAULT_TIMEOUT, reboot_wait=REBOOT_WAIT):
    """Log in, read the current version, upload, wait for reboot, report the new version.

    Returns 0 on success, non-zero on failure (usable as a process exit code)."""
    if not host:
        print("push: no bridge host - set custom_push_host in platformio.local.ini, "
              "OBI_BRIDGE_HOST, or pass --host", file=sys.stderr)
        return 2
    if not os.path.isfile(firmware):
        print("push: firmware not found: %s" % firmware, file=sys.stderr)
        return 2

    base = _base(host)
    size = os.path.getsize(firmware)
    print("push: target %s" % base)
    print("push: image  %s (%d KB)" % (firmware, size // 1024))

    # 1) authenticate (skipped automatically if no user given / auth disabled on the device)
    try:
        cookie = _login(base, user, password, timeout)
    except urllib.error.HTTPError as e:
        if e.code == 401:
            print("push: login rejected (wrong user/password)", file=sys.stderr)
            return 3
        print("push: login failed: HTTP %s" % e.code, file=sys.stderr)
        return 3
    except urllib.error.URLError as e:
        print("push: cannot reach %s: %s" % (base, e.reason), file=sys.stderr)
        return 3
    if user and not cookie:
        # credentials were supplied but the device handed back no session — it may be running open;
        # we continue, and the upload itself will tell us if auth is actually required.
        print("push: note - no session cookie returned (bridge may have auth disabled)")

    # 2) current version (best effort — needs auth if the bridge is locked)
    try:
        before = _fw_version(_status(base, cookie, timeout))
        print("push: current firmware: %s" % before)
    except Exception:
        before = None

    # 3) upload
    print("push: uploading to /api/selfupdate ...")
    try:
        resp = _upload(base, cookie, firmware, max(timeout, 60))
    except urllib.error.URLError as e:
        print("push: upload failed: %s" % e.reason, file=sys.stderr)
        return 4
    if not resp.get("ok"):
        print("push: device REJECTED the image (ok=false). "
              "If the bridge has a web user set, provide credentials.", file=sys.stderr)
        return 4
    print("push: image accepted - device is rebooting into the new firmware")

    # 4) wait for it to come back, then report the running version
    deadline = time.time() + reboot_wait
    time.sleep(3)                        # give it a moment to actually restart
    while time.time() < deadline:
        try:
            cookie2 = _login(base, user, password, timeout)   # sessions are wiped by the reboot
            after = _fw_version(_status(base, cookie2, timeout))
            print("push: back up - running firmware: %s" % after)
            if before and after and before != "?" and after != "?":
                print("push: %s  ->  %s" % (before, after))
            print("push: done.")
            return 0
        except Exception:
            time.sleep(2)
    print("push: image was accepted, but the device did not answer within %ds. "
          "It is likely still booting — check %s manually." % (reboot_wait, base))
    return 0                             # upload succeeded; only the post-check timed out


# --------------------------------------------------------------------------- standalone CLI
def _main(argv=None):
    import argparse
    p = argparse.ArgumentParser(description="OTA-push a firmware.bin to a running OBI gateway/bridge.")
    p.add_argument("--host", default=os.environ.get("OBI_BRIDGE_HOST", ""),
                   help="bridge IP or hostname (env: OBI_BRIDGE_HOST)")
    p.add_argument("--user", default=os.environ.get("OBI_BRIDGE_USER", ""),
                   help="web user, if auth is enabled (env: OBI_BRIDGE_USER)")
    p.add_argument("--pass", dest="password", default=os.environ.get("OBI_BRIDGE_PASS", ""),
                   help="web password (env: OBI_BRIDGE_PASS)")
    p.add_argument("--firmware", default=".pio/build/obi_gateway_c3/firmware.bin",
                   help="path to the firmware .bin to upload")
    p.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT, help="per-request timeout (s)")
    p.add_argument("--reboot-wait", type=int, default=REBOOT_WAIT,
                   help="how long to wait for the device to come back (s)")
    a = p.parse_args(argv)
    return push(a.host, a.user, a.password, a.firmware, a.timeout, a.reboot_wait)


# --------------------------------------------------------------------------- PlatformIO integration
# When PlatformIO loads this as an extra_script, `Import` is injected into globals and `env` is available.
# When run as a plain program, `Import` is undefined -> we fall through to the CLI.
try:
    Import("env")                        # noqa: F821  (injected by PlatformIO/SCons)
    _UNDER_PIO = True
except NameError:
    _UNDER_PIO = False

if _UNDER_PIO:
    def _pio_push(target, source, env):  # noqa: F811
        host = env.GetProjectOption("custom_push_host", "") or os.environ.get("OBI_BRIDGE_HOST", "")
        user = env.GetProjectOption("custom_push_user", "") or os.environ.get("OBI_BRIDGE_USER", "")
        pw   = env.GetProjectOption("custom_push_pass", "") or os.environ.get("OBI_BRIDGE_PASS", "")
        firmware = env.subst("$BUILD_DIR/${PROGNAME}.bin")
        rc = push(host, user, pw, firmware)
        if rc:
            env.Exit(rc)                 # fail the target so `pio run -t push` reports the error

    env.AddCustomTarget(                 # noqa: F821
        name="push",
        dependencies="$BUILD_DIR/${PROGNAME}.bin",   # builds the firmware first, then runs the push
        actions=[_pio_push],
        title="OTA Push",
        description="Build + OTA-push firmware.bin to the configured bridge (/api/selfupdate)")
elif __name__ == "__main__":
    sys.exit(_main())
