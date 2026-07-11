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

// --- entry_sxth16_fix ----------------------------------------------------
// Fixes a missing sign-extension in sub_C0A4's SML TL=0x53 (Integer16)
// case, confirmed via IDA trace (2026-07-11): the adjacent TL=0x63 case
// does `sxth r0,r0` before computing the sign (`asrs r1,r0,#0x1f`); the
// 0x53 case is missing that one instruction, so R0 (the zero-extended
// 16-bit value from the two LDRBs just before this point) always reads as
// non-negative downstream -- e.g. raw bytes FE 5F (-417 decimal, correctly
// signed on the wire) end up reported as +65119 instead of -417, not
// merely a flipped sign but the value reinterpreted as unsigned. This is a
// general firmware defect (any meter reporting a negative value via a
// plain SML Integer16 field hits it), not specific to one meter model, so
// unlike the FIX_NEGATIVE_POWER-guarded hooks below, this one is built
// into every variant unconditionally.
//
// Replaces the 4-byte `asrs r1,r0,#0x1f ; str r1,[r4,#4]` at 0xC110 inside
// sub_C0A4. This is NOT a jump-table-slot replacement (unlike entry_int24
// above) -- it's a straight-line mid-function patch, so it must redo the
// `str r1,[r4,#4]` it removes from the original flow, then return via
// `bx lr` (not `pop`) so execution resumes at 0xC114 (`str r0,[r4]`,
// followed by the original, unmodified `b loc_C26C`). No internal `bl`
// here, so LR (== 0xC114, set by the `bl` that reached us) is never
// clobbered -- confirmed via trace that LR is treated as pure scratch
// mid-function in sub_C0A4 (it's only ever restored via `pop {r3-r7,pc}`
// at the function's real exits, never read as a live register in between).
.section .text.entry_sxth16_fix, "ax", %progbits
.global entry_sxth16_fix
.thumb_func
entry_sxth16_fix:
    sxth r0, r0            // the missing instruction: sign-extend bit15->31
    asrs r1, r0, #0x1f      // now correctly propagates the sign to the high dword
    str  r1, [r4, #4]       // redo the store removed from the original flow
    bx   lr                 // -> sub_C0A4 @ 0xC114 (str r0,[r4]; b loc_C26C)

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
