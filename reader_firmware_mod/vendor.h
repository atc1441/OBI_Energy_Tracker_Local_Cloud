// vendor.h — known addresses/prototypes inside reader_meter_v57.bin, filled
// in as we reverse-engineer more of the vendor firmware. Nothing here yet
// is required by the int24 proof-of-concept hook; this is the place future
// hooks (e.g. the IR-debug-over-LoRa feature) will declare the LoRa TX
// routine, the IR frame buffer, etc. once located in IDA.
#pragma once

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef int            i32;

// Example of how a vendor function will be declared once its address is
// known (Thumb functions have address|1 in a real function pointer, but
// since we call these only via inline asm "bl" in entry.S, plain #define
// addresses are enough there; this header is for hooks.c call-outs):
//
//   typedef void (*lora_tx_fn)(const u8 *payload, u32 len, u8 handle[3]);
//   #define vendor_lora_tx ((lora_tx_fn)0x12345)
