/* STM32F405: a Cortex-M4F with 1 MB of flash and 128 KB of SRAM (the 64 KB
 * of CCM is left alone). The world's constant half lives in the flash; the
 * half that changes, and the stacks, fit in the SRAM. */
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}
INCLUDE link_common.ld
