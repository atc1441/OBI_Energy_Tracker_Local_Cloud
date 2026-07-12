# UART config protocol (`C5 5C`) — the self-hosting entry point

The bridge's UART0 console (**115200 8N1**, GPIO20 RX / GPIO21 TX) carries a **plaintext** config
channel in addition to the log output. It is the fastest way to read the TEA key and re-point WiFi on a
gateway you physically own. It is **not** reachable over BLE (the transport fixes the pipe id).

## Frame format
```
C5 5C | LEN(2, big-endian) | CRC(2) | FE | CMD | PAYLOAD…
```
- `LEN` = total byte count of the frame (from `C5`).
- `CRC` = **CRC-16/MODBUS** (init `0xFFFF`) over everything from `FE` onward; sending `00 00` **skips**
  the check. On the wire the two CRC bytes are `[lo][hi]`.
- `FE` = protocol marker (254). `CMD` = one of the six below.
- Response: same framing, with `CMD | 0x80`, and the handler blob as payload.

## Commands
| cmd | dir | request payload | response payload |
|---|---|---|---|
| **48** `0x30` | write | 38 B: `UUID(16) + BLE-ID(6) + TEA key(16)` | ack |
| **49** `0x31` | read | — | 39 B: `marker(1) + UUID(16) + BLE-ID(6) + TEA key(16)` |
| **52** `0x34` | write | `[N][N bytes]` → sends a LoRa test frame | ack |
| **55** `0x37` | read | — | WiFi status + SSID |
| **58** `0x3A` | write | `[ssid_len][pwd_len][ssid][password]` | ack |
| **59** `0x3B` | read | — | `[status][ssid_len][pwd_len][SSID][PASSWORD]` |

> ⚠️ **Stock firmware 1.0.1 caveat:** `pwd_len` is a single byte (wire capacity up to 255), but the stock
> gateway firmware itself only handles WiFi passwords up to **32 bytes** — a longer password is silently
> truncated/rejected and the device fails to join. Keep the WiFi password ≤32 characters when provisioning
> a stock (non-custom) gateway over cmd 58.

## Ready-made frames
```
Read TEA key + IDs (cmd 49):   C5 5C 00 08 00 00 FE 31         # CRC 0000 = skip
Read WiFi credentials (cmd 59): C5 5C 00 08 00 00 FE 3B
Set TEA key (cmd 48, 38B):      C5 5C 00 2E 00 00 FE 30 <16B UUID><6B BLE-ID><16B TEA KEY>
```
Response to cmd 49 (example, all placeholders):
`marker · UUID=0011…EEFF · BLE-ID="OBIXXX" · TEA key=00112233445566778899AABBCCDDEEFF`.

## Browser tool
Open [../06-tools/obi_uart_config.html](../06-tools/obi_uart_config.html) in Chrome/Edge (Web Serial):
one-click reads for cmds 49/55/59, builders for 48/52/58, automatic CRC and response decode.

## Why this matters for self-hosting
- **cmd 49** gives you the device TEA key without touching the cloud (physical access required).
- **cmd 48** lets you *set* a known key (e.g. to align app + your tooling).
- **cmd 58/59** read/write WiFi credentials directly.

> Also note (boot log): the firmware reads a 16-byte eFuse USER_DATA value at startup and prints it to
> the same UART log ("Key content"). Capture the boot log if you are studying key provenance.
