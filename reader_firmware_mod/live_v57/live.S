// live.S — v57/v90 reader: ~1 s readings in live mode.
// Addresses use the repo numbering (file offset + 0x4000); the
// image runs at +0x800 (vector table at file offset 0).
//
// Two things hold a v57 reader back in live mode:
//  1. Meter read cycle (as on legacy v32): take one telegram, then wait
//     in state 0x168 until (now_s - start_s) > period (upload interval).
//  2. LoRa window: the reader only transmits in a window it opens
//     9950 ms after the last beacon it heard, i.e. every ~10 s.
// The hooks skip the pause and shorten the window to 950 ms (every
// 1 Hz beacon), but only in live mode. Other intervals stay stock.
//
// Position independent (no relocations): patch_live.py appends the
// blob to any v90 image, 4-byte aligned for the literal pool.
.syntax unified
.thumb

.equ STOCK_CHECK, 0xB7C8 + 0x800 + 1   // sub_B7C8, runtime, Thumb
.equ INTERVAL,    0x20000114           // upload interval (s), set by ACK
.equ WIN_STOCK,   9950                 // ms after last beacon
.equ WIN_LIVE,    950

.section .text.live, "ax", %progbits

// Replaces `bl sub_B7C8` at 0xD11E.
// r0 = cycle start (s), r1 = period (s).
.global entry_cycle_due
.thumb_func
entry_cycle_due:
    cmp  r1, #2              // live mode: no pause
    bhi  1f
    movs r0, #1
    bx   lr
1:  ldr  r3, =STOCK_CHECK    // tail jump, lr intact
    bx   r3

// Replaces `ldr r1,=9950; movs r0,#0` at 0xB942.
.global entry_window_base
.thumb_func
entry_window_base:
    ldr  r1, =WIN_STOCK
    ldr  r3, =INTERVAL
    ldr  r3, [r3]
    subs r3, #1              // live: interval 1 or 2
    cmp  r3, #1
    bhi  2f
    ldr  r1, =WIN_LIVE
2:  movs r0, #0
    bx   lr

// Replaces `ldr r1,=9950; adds r0,r0,r1` at 0xB906.
// r0 = retries * 1000 ms.
.global entry_window_retry
.thumb_func
entry_window_retry:
    ldr  r1, =WIN_STOCK
    ldr  r3, =INTERVAL
    ldr  r3, [r3]
    subs r3, #1              // live: interval 1 or 2
    cmp  r3, #1
    bhi  3f
    ldr  r1, =WIN_LIVE
3:  adds r0, r0, r1
    bx   lr

.ltorg
