/* link.ld — places our hook code in the free flash space right after
 * reader_meter_v57.bin (44552 bytes @ base 0x4000 -> next free byte at
 * 0x4000+44552 = 0xEE08). Budget kept modest (4 KB) until we've confirmed
 * the real flash size limit; bump LENGTH here if a future hook needs more
 * room (and re-check against actual OTA/flash behavior before relying on
 * it).
 */
MEMORY
{
  HOOKS (rx) : ORIGIN = 0xEE08, LENGTH = 0x1000
}

ENTRY(entry_int24)

SECTIONS
{
  .text : {
    KEEP(*(.text.entry_int24))
    KEEP(*(.text.entry_sxth16_fix))
    KEEP(*(.text.entry_power_correct))
    KEEP(*(.text.entry_power_correct2))
    *(.text*)
    *(.rodata*)
  } > HOOKS

  /DISCARD/ : {
    *(.ARM.exidx*)
    *(.ARM.extab*)
    *(.comment)
    *(.note*)
    *(.ARM.attributes*)
  }
}
