/* link.ld — legacy v32 live hook.
 *
 * The v32 image ends at runtime 0xFBC4 and the bootloader rejects images
 * that reach the last flash page (0xFC00), so nothing can be appended.
 * The hook goes into dead code instead: uECC's vli_mmod_fast_secp256k1
 * (repo numbering 0xE6EC-0xE773). Only its curve struct points to it,
 * and nothing references that struct; the reader only uses secp256r1.
 */
MEMORY
{
  CAVE (rx) : ORIGIN = 0xE6EC, LENGTH = 136
}

ENTRY(entry_cycle_due)

SECTIONS
{
  .text : { KEEP(*(.text.entry_cycle_due)) *(.text*) } > CAVE

  .vendor 0xC074 (NOLOAD) : { KEEP(*(.vendor_sub_C074)) }   /* stock code, call target only */

  /DISCARD/ : {
    *(.ARM.exidx*)
    *(.ARM.extab*)
    *(.comment)
    *(.note*)
    *(.ARM.attributes*)
  }
}
