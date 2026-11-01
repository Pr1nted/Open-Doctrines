/* Reset, and the vector table, for any Cortex-M board here.
 *
 * Copies .data out of flash, clears .bss, turns the FPU on where there is one
 * (the CM4F port saves its registers on a context switch, and would fault on
 * a core that still has it off), and calls main. The three exceptions the
 * scheduler needs point straight at the port's handlers, as FreeRTOS V11's
 * configCHECK_HANDLER_INSTALLATION expects. No peripheral interrupts: the
 * console polls (freertos/core/board.h).
 */
#include <stdint.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern int main(void);
extern void vPortSVCHandler(void);
extern void xPortPendSVHandler(void);
extern void xPortSysTickHandler(void);
void board_exit(int code);

void Reset_Handler(void)
{
    uint32_t *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata) *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss;) *dst++ = 0;
#if defined(__ARM_FP)
    *(volatile uint32_t *)0xE000ED88 |= 0xFu << 20;    /* CPACR: CP10, CP11 full access */
    __asm volatile("dsb\n isb");
#endif
    main();
    for (;;) {
    }
}

static void Fault_Handler(void)
{
    board_exit(6);
    for (;;) {
    }
}

__attribute__((section(".isr_vector"), used))
void (*const vectors[16])(void) = {
    (void (*)(void))&_estack,
    Reset_Handler,
    Fault_Handler,       /* NMI */
    Fault_Handler,       /* HardFault */
    Fault_Handler,       /* MemManage */
    Fault_Handler,       /* BusFault */
    Fault_Handler,       /* UsageFault */
    0, 0, 0, 0,
    vPortSVCHandler,
    Fault_Handler,       /* DebugMon */
    0,
    xPortPendSVHandler,
    xPortSysTickHandler,
};
