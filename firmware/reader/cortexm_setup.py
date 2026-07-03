# cortexm_setup.py — run in IDA (File > Script file) after opening a reader_v*.elf or .bin
# Adds the exact BAT32G135 memory map, forces Thumb on the flash, and parses the vector
# table at 0x0 to create + name the exception/IRQ handlers.
#
# Target: BAT32G135 (Singapore Changi Technology / CMSemicon) — ARM Cortex-M0+ (ARMv6-M,
# Thumb-only) up to 64 MHz. Code flash 64KB @0x0, data flash 1.5KB @0x500000, SRAM 8KB @0x20000000.

import ida_segment, ida_bytes, ida_funcs, ida_name, ida_segregs, idc, idaapi

FLASH_END = 0x00020000   # accept handlers anywhere in the loaded flash image (>=64KB for 1.2.x)

def add_seg(start, end, name, clazz, perm):
    if ida_segment.get_segm_by_name(name):
        return
    s = ida_segment.segment_t()
    s.start_ea = start; s.end_ea = end
    s.bitness = 1  # 32-bit
    s.perm = perm
    ida_segment.add_segm_ex(s, name, clazz, ida_segment.ADDSEG_NOSREG)

def ensure_map():
    # BAT32G135 map: data flash, SRAM (8KB), peripherals, and the Cortex-M System Control Space.
    # (Code flash @0x0 is already mapped by the loaded image.)
    add_seg(0x00500000, 0x00500600, "DATAFLASH", "DATA", ida_segment.SEGPERM_READ)               # 1.5KB special
    add_seg(0x20000000, 0x20002000, "SRAM",      "DATA", ida_segment.SEGPERM_READ|ida_segment.SEGPERM_WRITE)  # 8KB
    add_seg(0x40000000, 0x40060000, "PERIPH",    "DATA", ida_segment.SEGPERM_READ|ida_segment.SEGPERM_WRITE)
    add_seg(0xE0000000, 0xE0100000, "SCS",       "DATA", ida_segment.SEGPERM_READ|ida_segment.SEGPERM_WRITE)

def force_thumb():
    # set the T flag = 1 across the flash code segment so everything decodes as Thumb
    seg = ida_segment.getseg(0x0)
    if not seg:
        return
    treg = ida_segregs.str2reg("T")
    idc.split_sreg_range(seg.start_ea, treg, 1, idaapi.SR_user)

# ARMv6-M (Cortex-M0+) exception names for the fixed part of the table
EXC = {1:"Reset", 2:"NMI", 3:"HardFault", 11:"SVC", 14:"PendSV", 15:"SysTick"}

def parse_vectors():
    sp = ida_bytes.get_dword(0x0)
    idc.set_cmt(0x0, "Initial SP = 0x%08X" % sp, 0)
    ida_name.set_name(0x0, "g_vector_table", ida_name.SN_NOWARN|ida_name.SN_FORCE)
    made = 0
    # walk the table until we leave plausible handler values (0 gaps allowed)
    i = 1
    zeros = 0
    while i < 64:
        v = ida_bytes.get_dword(i*4)
        if v == 0:
            zeros += 1
            if zeros > 24:   # long run of zeros -> end of table
                break
            i += 1; continue
        if (v & 1) == 1 and v < FLASH_END:      # Thumb handler in flash
            tgt = v & ~1
            ida_bytes.del_items(tgt, ida_bytes.DELIT_SIMPLE, 2)
            if idc.create_insn(tgt) and ida_funcs.add_func(tgt):
                made += 1
            nm = EXC.get(i, "IRQ%d_Handler" % (i-16) if i >= 16 else "Vector%d" % i)
            # only name if it still has a default name
            cur = ida_name.get_name(tgt)
            if not cur or cur.startswith("sub_") or cur.startswith("loc_"):
                ida_name.set_name(tgt, nm, ida_name.SN_NOWARN|ida_name.SN_FORCE)
        i += 1
    return made

def main():
    ensure_map()
    force_thumb()
    n = parse_vectors()
    idaapi.auto_wait()
    print("[cortexm_setup] memory map added, Thumb forced, %d vector handlers created." % n)

main()
