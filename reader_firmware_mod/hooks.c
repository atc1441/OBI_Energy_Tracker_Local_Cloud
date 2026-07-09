// hooks.c — hook bodies, compiled as normal freestanding C (AAPCS calling
// convention). Register-convention glue lives in entry.S, not here.
#include "vendor.h"

// int24 (SML TL=0x54) decoder — the reader_meter_v57.bin firmware is
// missing this case in its generic SML value-type switch (sub_C0A4),
// which is why power (16.7.0) shows n/a whenever the meter's value needs
// 3 bytes to encode. This restores it, compiled from C instead of the
// hand-assembled Thumb-1 trampoline used for the very first fix (proven
// byte-for-byte equivalent via IDA diff before this pipeline replaced it).
//
//   p   = pointer to the TL byte of the value field (== R5+R2 at the
//         original call site)
//   out = pointer to the 10-byte output area (== R4 at the original call
//         site): out[0..3] = value (low dword), out[4..7] = sign dword,
//         out[8]/out[9] = the two extra bytes the caller also expects
//         (unit/scaler-adjacent bytes, copied from p-0x20+0x1D / +0x1F)
//
// Returns 1 (the vendor's "decode succeeded" convention for this switch
// case) -- entry.S puts this return value straight into R0 for the caller.
int hook_decode_int24(u8 *p, u8 *out)
{
    u32 v = ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
    i32 value = (i32)(v << 8) >> 8;      // sign-extend 24 -> 32
    i32 hi    = value >> 31;             // sign-extended hi dword

    out[0] = (u8)(value);
    out[1] = (u8)(value >> 8);
    out[2] = (u8)(value >> 16);
    out[3] = (u8)(value >> 24);
    out[4] = (u8)(hi);
    out[5] = (u8)(hi >> 8);
    out[6] = (u8)(hi >> 16);
    out[7] = (u8)(hi >> 24);

    u8 *q = p - 0x20;
    out[8] = q[0x1D];
    out[9] = q[0x1F];
    return 1;
}
