# ESP32-C3 radio bench

This example runs the actual `bthome_core.cpp` and `bthome_ble.cpp` on a plain
ESP32-C3 with native USB, without a LoRa transceiver. It injects simulated reader
values: +1234, 0 and -850 W, changing every ten seconds. It is not meter firmware
and must not be installed on an OBI gateway.

Back up existing firmware before flashing. Create `bench_secrets.h` beside
`main.cpp` (the file is ignored by Git):

```cpp
#pragma once
#define BENCH_BINDKEY "REPLACE_WITH_32_RANDOM_HEX_CHARACTERS"
#define BENCH_AP_PASSWORD "REPLACE_WITH_PRIVATE_WIFI_PASSWORD"
```

Generate a fresh key, for example with
`python3 -c 'import secrets; print(secrets.token_hex(16))'`. Use an AP password
of at least eight characters. Keep the key for your BTHome receiver. Build from
`open_obi_energy_meter`:

```sh
pio run -e bthome_c3_bench
pio run -e bthome_c3_bench -t upload --upload-port /dev/ttyACM0
pio device monitor --port /dev/ttyACM0 --baud 115200
```

The board advertises as `OBI-PWR`; serial output prints its Bluetooth MAC.
It also starts the password-protected AP `OBI-BLE-Test`.
`http://192.168.4.1/api/bthome` returns status without secrets.
The bench uses reader ID `010203` and a five-second freshness timeout.

Send these commands over USB serial, followed by a newline:

| Command | Effect |
| --- | --- |
| `power 1234`, `power 0`, `power -850` | Hold a simulated reading |
| `pause` | Stop new samples; BLE expires after five seconds |
| `resume` | Resume sampling |
| `auto` | Resume the ten-second value cycle |
| `wifi off`, `wifi on` | Disable/enable the test AP |
| `reboot` | Restart and reserve a new counter block |

Free/minimum heap and Wi-Fi mode are logged every five seconds.
Configuration uses the same NVS namespaces as production. Rebooting retains the
counter high-water mark; do not erase NVS or restore old backups with the same
bindkey. Use a new key after either operation.

Acceptance requires receiving actual advertisements and independently checking
authenticated decryption, signed power, stale-data shutdown and monotonically
increasing counters across restart. This validates the BLE path on a generic C3.
LoRa reception, OBI OTA installation, full gateway memory use and operation under
network traffic still require the real OBI gateway and reader.
