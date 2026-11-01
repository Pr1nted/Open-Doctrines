/* Machine mode from the first instruction: no firmware, no supervisor.
 * Set the global and stack pointers, point traps at FreeRTOS, clear .bss,
 * and call main. */
    .section .text.init
    .globl _start
_start:
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    la sp, _stack_top
    la t0, freertos_risc_v_trap_handler
    csrw mtvec, t0
    la t0, __bss_start
    la t1, __bss_end
1:  bgeu t0, t1, 2f
    sw zero, 0(t0)
    addi t0, t0, 4
    j 1b
2:  call main
3:  wfi
    j 3b
