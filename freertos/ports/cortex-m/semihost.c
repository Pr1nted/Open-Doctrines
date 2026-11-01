/* Leaving QEMU: semihosting's SYS_EXIT, which QEMU answers by exiting with
 * the code given when it runs with -semihosting. On a real board with no
 * debugger attached a BKPT would halt the core, so board_exit on hardware
 * never gets here (see each board's board_exit). */
#include <stdint.h>

void semihost_exit(int code)
{
    /* ADP_Stopped_ApplicationExit, with the code: the extended form, so the
     * exit status reaches the shell rather than always reading 0. */
    static uint32_t block[2];
    block[0] = 0x20026;
    block[1] = (uint32_t)code;
    register uint32_t r0 __asm__("r0") = 0x20;          /* SYS_EXIT_EXTENDED */
    register uint32_t r1 __asm__("r1") = (uint32_t)block;
    __asm volatile("bkpt 0xab" : : "r"(r0), "r"(r1) : "memory");
}
