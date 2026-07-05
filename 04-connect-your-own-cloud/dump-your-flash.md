# Dump the whole flash from your own device (and split out the OTA partitions)

The [cloud OTA download](tools/obi_ota_download.py) gets you the *app image the vendor serves*, but it
needs a provisioning cert and is fiddly. Once this repo's **custom firmware is running** on the bridge
there is a second, much simpler route: the firmware's **web debug page can read the device's own SPI
flash** — the whole chip, including *both* OTA app slots. That means you can pull:

- the **currently running** firmware,
- the **previous version** still sitting in the other OTA slot (whatever the last update replaced), and
- the bootloader, partition table, NVS and data partitions.

No esptool, no UART, no cert — just a browser. (The **stock** firmware has no such page — this only works
after you've flashed the custom firmware. The stock image itself is obtained the [cloud way](README.md#flash-your-own-firmware).)

> ⚠️ You are on the raw flash. **Reading** is harmless. Do **not** use Write / Erase / the hex editor on
> the debug page unless you know the exact offset — a wrong write bricks the device. This guide only reads.

---

## 1. Open the flash debug page

1. Make sure the bridge is on your WiFi and you can reach its web UI (`http://<device-ip>/`).
2. Click **🐞 Debug** (top right), or go straight to `http://<device-ip>/debug`.

The bar at the top has an address field, a **partition dropdown**, **Save .bin**, **Dump full flash**
and **eFuses**.

## 2. Read the partition map first

Open the **— partition —** dropdown. It lists every partition on *this* device with its label, offset and
size, e.g.:

```
nvs      @0x9000 · 20K
otadata  @0xe000 · 8K
app0     @0x10000 · 1920K      <- OTA slot 0  (firmware)
app1     @0x1f0000 · 1920K     <- OTA slot 1  (firmware)
spiffs   @0x3d0000 · 128K
```

Write these numbers down — they are what you'll split by. (The exact offsets/sizes are fixed per device;
the two app slots may be labelled `app0`/`app1` or `ota_0`/`ota_1`.) You can also get the same table from
the UART `info` command or from `GET http://<device-ip>/api/flash/info`.

Which app slot is *running* vs *previous*: the running one is the boot partition; the other holds the image
from before the last OTA. If you don't know which is which, just grab both — you'll have both versions.

---

## Option A — pull one partition directly (easiest, no splitting)

Do this per OTA slot and you're done — the debug page can save a single partition for you:

1. In the partition dropdown, select the slot you want (e.g. `app0`).
2. Click **Save .bin**. It asks *"Bytes to read & save from 0x…"* and pre-fills "to end of flash".
   **Replace that with the partition's size in bytes** so you only grab that one partition. From the
   example above a 1920K slot is `1920 × 1024 = 1966080`.
3. It downloads `flash_0x<offset>_<size>.bin` — that partition alone.

Repeat for `app1` to get the second (previous) version. Each file is a plain ESP32-C3 app image (starts
with `0xE9`) — ready to re-serve with [`mqtts_server.py --ota-firmware …`](README.md#flash-your-own-firmware),
load into Ghidra/IDA, or archive as a backup.

---

## Option B — dump the whole flash, then split (what to share)

If you want *everything* in one shot (backup, or to study the layout):

1. On `/debug`, click **Dump full flash** and confirm. It reads the entire chip — bootloader + partition
   table + both apps + NVS — and downloads `flash_full_<size>k.bin` (e.g. `flash_full_4096k.bin` = 4 MB).
   **This is slow over WiFi** (a few minutes for 4 MB); leave the tab open until the download appears.

This one file is the raw chip image. To get the **OTA partitions as separate files**, split it by the
offsets from step 2. Easiest is the included splitter, which reads the partition table *out of the dump
itself* (no need to type offsets):

```bash
# lists the partitions, then writes ota_0.bin, ota_1.bin, nvs.bin, … next to it:
python 06-tools/split_flash_dump.py flash_full_4096k.bin -o out/

# just the two firmware slots:
python 06-tools/split_flash_dump.py flash_full_4096k.bin -o out/ --only app0,app1

# only inspect the table, write nothing:
python 06-tools/split_flash_dump.py flash_full_4096k.bin --list
```

You get one `<label>.bin` per partition — `app0.bin` / `app1.bin` are the two firmware versions.

### Splitting by hand (dd)

If you'd rather not use the script, cut it with `dd` using the offset/size **in bytes** you noted in
step 2 (decimal here; convert the hex offsets, e.g. `0x10000 = 65536`, `1920K = 1966080`):

```bash
dd if=flash_full_4096k.bin of=app0.bin bs=1 skip=65536  count=1966080
dd if=flash_full_4096k.bin of=app1.bin bs=1 skip=2031616 count=1966080
```

(`bs=1` is simple but slow; for a faster cut use `bs=4096` and divide the byte offsets/sizes by 4096 for
`skip`/`count`, since every partition here is 4 KB-aligned.)

---

## What each file is good for

| File | What it is | Use |
|---|---|---|
| `app0.bin` / `app1.bin` | full ESP32-C3 app images (`0xE9` magic) | re-flash via cloud OTA, reverse-engineer, keep as backup |
| `nvs.bin` | non-volatile settings | holds WiFi creds, cloud certs, TEA key at runtime (not the app image) |
| `flash_full_*.bin` | the raw 4 MB chip | complete backup; re-split any time |

To load an app image in IDA/Ghidra you still have to map its segments — see
[../03-reverse-engineering/firmware-layout.md](../03-reverse-engineering/firmware-layout.md). To re-flash
one to a device on your own cloud, see [Flash your own firmware](README.md#flash-your-own-firmware).

> **Copyright note:** the extracted stock bridge/reader images are SUMEC / OBI copyright. Dumping *your
> own* device for backup/study is fine; don't redistribute the vendor binaries — this repo ships only
> tools and docs, never the images (see [../firmware/README.md](../firmware/README.md)).
