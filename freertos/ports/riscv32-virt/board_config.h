/* QEMU virt's CLINT: mtime counts at 10 MHz whatever the core does, and the
 * RISC-V port sets the tick from it, so this is the TIMER's clock. */
#define configCPU_CLOCK_HZ           10000000
#define configMTIME_BASE_ADDRESS     0x0200BFF8UL
#define configMTIMECMP_BASE_ADDRESS  0x02004000UL
