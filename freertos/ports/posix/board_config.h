/* FreeRTOS's POSIX port runs each task on a pthread, whose stack must be at
 * least PTHREAD_STACK_MIN: words here are 8 bytes, so these are 32 KB. */
#define configCPU_CLOCK_HZ        1000000
#define configMINIMAL_STACK_SIZE  4096
#define OD_CONSOLE_STACK          4096
#define OD_ENGINE_STACK           4096
#define configTOTAL_HEAP_SIZE     (256 * 1024)
