# Disclaimer & Scope

This repository is for **interoperability, education, and security research on hardware you own or are
explicitly authorized to test**.

- **Own devices only.** Do not use any of this against devices, accounts, radios, or cloud services you
  do not own or have written permission to test.
- **No real secrets.** Every key, certificate, UUID, device ID and MAC in this repo is a **placeholder**
  (for example `00112233445566778899AABBCCDDEEFF`). Generate your own PKI with the provided tools.
- **No attack tooling against the vendor cloud.** The goal here is to run *your* device against *your*
  own infrastructure. Where the vendor cloud is mentioned it is only to explain the device's behaviour;
  obtaining a device key through the vendor is described as an *authorized-account* action.
- **Radio / RF law.** 868 MHz usage is region-regulated (e.g. EU ISM duty-cycle limits). Operate within
  your local rules and only with your own reader/bridge.
- **Firmware images — not distributed.** For safety, **no vendor firmware binaries** (`.bin`/`.elf`) are
  included in this repo; they are SUMEC/OBI copyright, removed and git-ignored. This repo ships only
  documentation and tools — dump the image from a device you own (see [firmware/README.md](firmware/README.md)).
  The app image contains no device-specific credentials anyway (those live in NVS at runtime).
- **No warranty.** Everything here is provided as-is. Flashing, re-provisioning, or RF experiments can
  brick hardware or violate warranties/laws — you are responsible for what you do with it.

If you are a rights holder and want something adjusted, open an issue.
