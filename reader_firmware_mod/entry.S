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

#ifdef FIX_NEGATIVE_POWER
// --- entry_power_correct -------------------------------------------------
// Replaces the 4-byte `BL sub_4700` at 0x75EE inside sub_7434
// (reader_meter_v57.bin) -- reached only on the branch that already
// matched OBIS "1-0:16.7.0*255" a few instructions earlier, with the raw
// decoded power value in R0. Must preserve LR across our own extra call
// (the replaced single BL only clobbered it transiently); push/pop it
// instead of relying on `bx lr`, since hook_power_correct's own `bl`
// already overwrites LR with an address inside this trampoline.
.equ vendor_sub_4700, 0x4700

.section .text.entry_power_correct, "ax", %progbits
.global entry_power_correct
.thumb_func
entry_power_correct:
    push {lr}
    bl   hook_power_correct   // r0 = raw value in -> corrected value out
    bl   vendor_sub_4700       // original call, now with the corrected r0
    pop  {pc}                  // return to sub_7434 at 0x75F2, r0 = sub_4700's result

// --- entry_power_correct2 -------------------------------------------------
// The OTHER meter-parser path (sub_77B4/sub_9ED8, "case 1" in sub_AA7C's
// dispatch) -- confirmed by a live diagnostic to be the one actually used
// on this reader (an unconditional sentinel in entry_power_correct never
// showed up; this path's data did instead). Replaces the 4-byte
// `LDR R1,=unk_20000D68 ; ADDS R1,#8` at 0x7800 inside sub_77B4, reached
// right after `BL sub_9ED8` -- R0 already holds sub_9ED8's fully-computed
// power value (this path has no separate scaler-conversion call like
// sub_7434's sub_4700; sub_9ED8 does it internally), about to be stored
// via the replayed R1 target + a `BL sub_43A6` right after we return.
.equ obi2_struct_base, 0x20000D68

.section .text.entry_power_correct2, "ax", %progbits
.global entry_power_correct2
.thumb_func
entry_power_correct2:
    push {lr}
    bl   hook_power_correct2  // r0 = sub_9ED8's result in -> corrected value out
    ldr  r1, =obi2_struct_base
    adds r1, #8
    pop  {pc}                  // return to sub_77B4 at 0x7804
#endif
