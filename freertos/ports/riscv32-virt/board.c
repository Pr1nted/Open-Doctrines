/* QEMU's RISC-V virt machine, RV32IMAC: an NS16550 UART at 0x10000000 for the
 * console, and the SiFive test device at 0x100000 to leave QEMU. RISC-V is
 * where small satellite computers are heading -- radiation-tolerant RV32
 * cores are on the market -- and this is that ISA on the same kernel. */
#include "board.h"

#include <stdint.h>

#define UART      ((volatile uint8_t *)0x10000000u)
#define UART_THR  0
#define UART_RBR  0
#define UART_LSR  5
#define LSR_DR    0x01
#define LSR_THRE  0x20
#define TESTDEV   (*(volatile uint32_t *)0x100000u)

const char board_name[] = "RISC-V virt (RV32IMAC)";
const int board_echo = 1;

void board_init(void)
{
}

int board_getc(void)
{
    return (UART[UART_LSR] & LSR_DR) ? UART[UART_RBR] : BOARD_NONE;
}

void board_putc(char c)
{
    while (!(UART[UART_LSR] & LSR_THRE)) {
    }
    UART[UART_THR] = (uint8_t)c;
}

void board_exit(int code)
{
    /* 0x5555 is a clean power-off; 0x3333 with the code above it a failure. */
    TESTDEV = code ? ((uint32_t)code << 16) | 0x3333 : 0x5555;
    for (;;) {
    }
}
