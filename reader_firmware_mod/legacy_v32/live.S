// live.S — legacy v32 reader: read every telegram in live mode.
// Addresses use the repo numbering (file offset + 0x4000).
//
// The meter read state machine (sub at 0xD624) runs in cycles: enable
// the meter UART, take one telegram, disable it, then wait in state
// 0x114 until (now_s - start_s) > period, where period is the upload
// interval. In live mode (interval 2) that pause alone costs 3 s, so a
// meter that pushes SML every second only gets read every 3-4 s.
// This hook makes the pause zero for interval <= 2. Longer intervals
// keep the stock behaviour (and the stock power saving).
.syntax unified
.thumb

// Stock wait check, typed Thumb symbol (binutils >= 2.45).
.section .vendor_sub_C074, "ax", %progbits
.thumb_func
vendor_sub_C074:

// Replaces `bl 0xC074` at 0xD7DE.
// r0 = cycle start (s), r1 = period (s).
.section .text.entry_cycle_due, "ax", %progbits
.global entry_cycle_due
.thumb_func
entry_cycle_due:
    cmp  r1, #2              // live mode: no pause
    bhi  1f
    movs r0, #1
    bx   lr
1:  push {lr}
    bl   vendor_sub_C074     // stock check
    pop  {pc}
