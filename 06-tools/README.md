# 06 · Tools

Standalone helpers. No real keys — every example value is a placeholder.

| Tool | Use |
|---|---|
| [`obi_uart_config.html`](obi_uart_config.html) | **Web Serial** UART config console: read/write TEA key & WiFi over the `C5 5C` protocol |
| [`obi_gateway_ble.html`](obi_gateway_ble.html) | **Web Bluetooth** gateway: push WiFi + certs (TEA+JSON) to the bridge |
| [`obi_ble_codec.py`](obi_ble_codec.py) | TEA frame codec: JSON ⇄ fragments ⇄ TEA (encode/decode BLE traffic) |
| [`split_flash_dump.py`](split_flash_dump.py) | Carve a full-flash dump (from the /debug "Dump full flash") into per-partition `.bin` files |

## obi_uart_config.html
Open in **Chrome or Edge** (Web Serial needs a Chromium browser; works from `file://`). Connect to the
bridge's UART0 (115200 8N1). One-click reads for cmd 49 (TEA key + IDs), 55 (WiFi status), 59 (WiFi
creds); builders for cmd 48/52/58. It computes the CRC-16/MODBUS, shows the frame byte-by-byte, and
auto-decodes responses (incl. the reassembled `C5 5C` frames out of the mixed log stream). Protocol:
[../03-reverse-engineering/uart-config-protocol.md](../03-reverse-engineering/uart-config-protocol.md).

## obi_gateway_ble.html
Open in Chrome/Edge (Web Bluetooth). Scans for `OBI-XXXXXX`, connects to service `ABF0`, and sends
TEA-encrypted JSON to `ABF2` (Status / WifiSet / SetTMPCertificate / Unbind). **You must fill in the
device's TEA key** — the field ships with a placeholder (`0011…EEFF`), not a real key. Use it as a
browser alternative to `ble_provision.py`.

## obi_ble_codec.py
Pure-stdlib reference codec (documented in
[../03-reverse-engineering/ble-protocol.md](../03-reverse-engineering/ble-protocol.md)):
```bash
# JSON -> encrypted frames (hex, for ABF2)
python obi_ble_codec.py encode --key <32hex> --data '{"type":"StatusRequest"}'
# captured ABF1 frames -> JSON
python obi_ble_codec.py decode --key <32hex> <framehex> [<framehex> ...]
# raw TEA-ECB block op
python obi_ble_codec.py tea-enc --key <32hex> --block 0011223344556677
```
Any key you pass is your own; the built-in example keys are placeholders.

## split_flash_dump.py
Pure-stdlib splitter for a **full-flash dump** you pulled from the custom firmware's `/debug` page
("Dump full flash" → `flash_full_*.bin`). It reads the ESP32 partition table at `0x8000` out of the dump
and writes each partition to its own file — so the two app slots (`ota_0`/`ota_1`, the running and the
previous firmware) come out separately.
```bash
python split_flash_dump.py flash_full_4096k.bin --list          # show the partition table only
python split_flash_dump.py flash_full_4096k.bin -o out/         # write every partition
python split_flash_dump.py flash_full_4096k.bin -o out/ --only app0,app1
```
Full walkthrough (dumping + sharing the partitions):
[../04-connect-your-own-cloud/dump-your-flash.md](../04-connect-your-own-cloud/dump-your-flash.md).

> Web Serial / Web Bluetooth need a secure context — a Chromium browser on desktop; both work from a
> local `file://`. If an embedded preview blocks port access, download and open the file locally.
