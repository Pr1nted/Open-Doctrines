/* Included by each board's linker script after it names FLASH and RAM. */
ENTRY(Reset_Handler)

SECTIONS
{
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
  .text : {
    *(.text*)
    *(.rodata*)
    . = ALIGN(4);
  } > FLASH
  .ARM.exidx : { *(.ARM.exidx*) } > FLASH
  _sidata = LOADADDR(.data);
  .data : {
    _sdata = .;
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > RAM AT > FLASH
  .bss (NOLOAD) : {
    _sbss = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(8);
    _ebss = .;
  } > RAM
  end = .;
  _end = .;
  _estack = ORIGIN(RAM) + LENGTH(RAM);
}
