// entry.S — hand-written glue trampolines. Each one adapts a specific
// vendor call-site's register convention to a normal AAPCS call into
// hooks.c, then returns the way the call site expects (often not a plain
// `bx lr`, since these are patched-in-place jump-table slots, not real
// function calls).
.syntax unified
.thumb

// --- entry_int24 -------------------------------------------------------
// Replaces the reader_meter_v57.bin case-2 (TL=0x54 / SML int24) default
// slot at 0xC0EA. On entry: r5+r2 = pointer to the value field's TL byte,
// r4 = output struct pointer. Must finish with `pop {r3-r7,pc}` to unwind
// sub_C0A4's own prologue (`push {r3-r7,lr}`) and return to ITS caller.
.section .text.entry_int24, "ax", %progbits
.global entry_int24
.thumb_func
entry_int24:
    adds r0, r5, r2   // r0 = p   (1st arg)
    mov  r1, r4       // r1 = out (2nd arg)
    bl   hook_decode_int24
    pop  {r3-r7, pc}  // r0 already holds hook_decode_int24's return value (1)
