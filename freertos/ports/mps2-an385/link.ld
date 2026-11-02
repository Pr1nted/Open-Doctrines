/* ARM's MPS2 board with the AN385 image: a Cortex-M3 at 25 MHz. Code in the
 * 4 MB SSRAM1 at 0, data in SSRAM2 at 0x20000000. */
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 4M
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
}
INCLUDE link_common.ld
