/* MPS2 AN385: Cortex-M3, and ARM's CMSDK UART0 for the console.
 *
 * The board QEMU's FreeRTOS demos have always used, which makes it the
 * reference here: if a transcript differs on another board, this one is
 * the one to believe. */
#include "board.h"

#include <stdint.h>

#define UART0       0x40004000u
#define UART_DATA   (*(volatile uint32_t *)(UART0 + 0x00))
#define UART_STATE  (*(volatile uint32_t *)(UART0 + 0x04))
#define UART_CTRL   (*(volatile uint32_t *)(UART0 + 0x08))
#define UART_BAUD   (*(volatile uint32_t *)(UART0 + 0x10))
#define TX_FULL     1u
#define RX_FULL     2u

const char board_name[] = "MPS2 AN385 (Cortex-M3, 25 MHz)";
const int board_echo = 1;

void semihost_exit(int code);

void board_init(void)
{
    UART_BAUD = 25000000 / 115200;
    UART_CTRL = 3;              /* TX and RX enable */
}

int board_getc(void)
{
    return (UART_STATE & RX_FULL) ? (int)(UART_DATA & 0xFF) : BOARD_NONE;
}

void board_putc(char c)
{
    while (UART_STATE & TX_FULL) {
    }
    UART_DATA = (uint8_t)c;
}

void board_exit(int code)
{
    semihost_exit(code);
}
