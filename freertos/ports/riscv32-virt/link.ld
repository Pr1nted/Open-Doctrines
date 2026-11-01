/* QEMU's RISC-V "virt" machine, 32-bit: everything in RAM at 0x80000000,
 * where QEMU loads the ELF when started with -bios none. */
OUTPUT_ARCH("riscv")
ENTRY(_start)
MEMORY
{
  RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 16M
}
SECTIONS
{
  .text : {
    KEEP(*(.text.init))
    *(.text*)
  } > RAM
  .rodata : { *(.rodata*) *(.srodata*) } > RAM
  .data : {
    *(.data*)
    __global_pointer$ = . + 0x800;
    *(.sdata*)
  } > RAM
  .bss (NOLOAD) : {
    . = ALIGN(16);
    __bss_start = .;
    *(.sbss*) *(.bss*) *(COMMON)
    . = ALIGN(16);
    __bss_end = .;
  } > RAM
  . = ALIGN(16);
  . += 4096;
  __freertos_irq_stack_top = .;
  . += 8192;
  _stack_top = .;
  end = .;
  _end = .;
  __heap_start = .;
  __heap_end = ORIGIN(RAM) + LENGTH(RAM);
}
