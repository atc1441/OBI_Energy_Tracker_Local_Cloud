#!/usr/bin/env python3
"""
fetch_tea_key.py — get a bridge's 16-byte TEA key from the OBI cloud.

For a device that is on YOUR OWN OBI account. Dead simple: run it and enter three things —
your OBI account email, your password, and the device's BLE name (OBI-XXXXXX).

    python fetch_tea_key.py

No third-party packages needed (Python standard library only).
"""
import json
import sys
import getpass
import urllib.request
import urllib.error

LOGIN_URL = "https://www.obi.de/regi/auth/api/public/login"
API       = "https://energy-tracking-backend.prod-eks.dbs.obi.solutions"


def _post(url, body, headers):
    data = json.dumps(body).encode()
    req = urllib.request.Request(
        url, data=data, method="POST",
        headers={"Content-Type": "application/json", "Accept-Encoding": "identity", **headers},
    )
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        sys.exit("HTTP %s from %s: %s" % (e.code, url, e.read().decode(errors="replace")[:200]))
    except urllib.error.URLError as e:
        sys.exit("network error: %s" % e.reason)


def get_tea_key(email, password, ble_name):
    # 1) log in -> JWT token
    tok = _post(LOGIN_URL,
                {"email": email, "password": password, "country": "DE"},
                {"x-app-type": "b2c", "x-obi-locale": "de-DE",
                 "User-Agent": "heyOBI APP / Android Phone 30"})
    token = tok.get("token")
    if not token:
        sys.exit("login failed: no token in response")

    # 2) challenge the device BLE name -> {"key": "<32 hex>"}
    res = _post(API + "/bluetooth-challenges",
                {"btChallengeId": ble_name},
                {"Authorization": "Bearer " + token,
                 "Accept": "application/vnd.obi.companion.energy-tracking.bluetooth-challenge.v1+json",
                 "User-Agent": "app_client"})
    return res.get("key")


def main():
    print("Fetch the TEA key for a bridge on YOUR OBI account.\n")
    email    = (sys.argv[1] if len(sys.argv) > 1 else input("OBI account email:    ")).strip()
    password = (sys.argv[2] if len(sys.argv) > 2 else getpass.getpass("OBI account password: "))
    ble_name = (sys.argv[3] if len(sys.argv) > 3 else input("Device BLE name (OBI-XXXXXX): ")).strip()

    key = get_tea_key(email, password, ble_name)
    if not key:
        sys.exit("no key returned (is the device on this account?)")
    print("\nTEA key for %s:\n  %s" % (ble_name, key))


if __name__ == "__main__":
    main()
