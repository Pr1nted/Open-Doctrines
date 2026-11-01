/* STM32F405, the microcontroller a class of CubeSat on-board computers is
 * built on. Under QEMU this is the Netduino Plus 2 machine; on a real board
 * the console is USART1 on PA9 (TX) and PA10 (RX), 115200 8N1.
 *
 * THE CLOCK. Out of reset a real F405 runs from its 16 MHz internal
 * oscillator, and that is what it is left on: no PLL, so nothing here can
 * hang waiting for a lock. QEMU's model runs at 168 MHz and does not model
 * the RCC at all -- reads of it come back zero -- so the oscillator's ready
 * bit tells the two apart, and the tick is right on both.
 */
#include "board.h"

#include <stdint.h>

#define REG(a)        (*(volatile uint32_t *)(a))
#define RCC_CR        REG(0x40023800u)
#define RCC_AHB1ENR   REG(0x40023830u)
#define RCC_APB2ENR   REG(0x40023844u)
#define GPIOA_MODER   REG(0x40020000u)
#define GPIOA_AFRH    REG(0x40020024u)
#define USART1_SR     REG(0x40011000u)
#define USART1_DR     REG(0x40011004u)
#define USART1_BRR    REG(0x40011008u)
#define USART1_CR1    REG(0x4001100Cu)
#define SR_RXNE       (1u << 5)
#define SR_TXE        (1u << 7)
#define HSIRDY        (1u << 1)

const char board_name[] = "STM32F405 (Cortex-M4F)";
const int board_echo = 1;
uint32_t SystemCoreClock = 16000000;

void semihost_exit(int code);

void board_init(void)
{
    if (!(RCC_CR & HSIRDY)) SystemCoreClock = 168000000;    /* QEMU */
    RCC_AHB1ENR |= 1u << 0;                     /* GPIOA */
    RCC_APB2ENR |= 1u << 4;                     /* USART1 */
    GPIOA_MODER = (GPIOA_MODER & ~(0xFu << 18)) | (0xAu << 18);   /* PA9, PA10: alternate */
    GPIOA_AFRH = (GPIOA_AFRH & ~(0xFFu << 4)) | (0x77u << 4);     /* AF7: USART1 */
    USART1_BRR = (16000000 + 115200 / 2) / 115200;                /* APB2 = HSI; QEMU ignores it */
    USART1_CR1 = (1u << 13) | (1u << 3) | (1u << 2);              /* UE, TE, RE */
}

int board_getc(void)
{
    return (USART1_SR & SR_RXNE) ? (int)(USART1_DR & 0xFF) : BOARD_NONE;
}

void board_putc(char c)
{
    while (!(USART1_SR & SR_TXE)) {
    }
    USART1_DR = (uint8_t)c;
}

void board_exit(int code)
{
    /* Only QEMU can be left; a satellite stays up. */
    if (SystemCoreClock != 16000000) semihost_exit(code);
}
