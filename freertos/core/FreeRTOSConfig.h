/* One configuration for every board; what differs is in the board's
 * board_config.h, which comes first and wins. */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include "board_config.h"

#define configUSE_PREEMPTION                1
#define configUSE_IDLE_HOOK                 0
#define configUSE_TICK_HOOK                 0
#define configTICK_RATE_HZ                  1000
#define configMAX_PRIORITIES                5
#define configMAX_TASK_NAME_LEN             10
#define configUSE_16_BIT_TICKS              0
#define configUSE_MUTEXES                   0
#define configUSE_COUNTING_SEMAPHORES       0
#define configUSE_TASK_NOTIFICATIONS        1
#define configUSE_TRACE_FACILITY            1
#define configUSE_TIMERS                    0
#define configCHECK_FOR_STACK_OVERFLOW      2
#define configUSE_MALLOC_FAILED_HOOK        1
#define configSUPPORT_DYNAMIC_ALLOCATION    1
#define configSUPPORT_STATIC_ALLOCATION     0
#define configQUEUE_REGISTRY_SIZE           0

#ifndef configTOTAL_HEAP_SIZE
#define configTOTAL_HEAP_SIZE               (12 * 1024)
#endif
#ifndef configMINIMAL_STACK_SIZE
#define configMINIMAL_STACK_SIZE            256
#endif
/* The console formats into stack buffers; the engine's deepest frame is the
 * AI choosing a war. Both measured with "hk" and given room past that. */
#ifndef OD_CONSOLE_STACK
#define OD_CONSOLE_STACK                    768
#endif
#ifndef OD_ENGINE_STACK
#define OD_ENGINE_STACK                     256
#endif

/* Cortex-M only. No interrupt here calls the kernel (the console polls), so
 * these only have to be legal: the kernel's at the lowest priority, and the
 * ceiling for API-calling ISRs in the middle. */
#define configKERNEL_INTERRUPT_PRIORITY      255
#define configMAX_SYSCALL_INTERRUPT_PRIORITY 0x40

#define INCLUDE_vTaskDelay                  1
#define INCLUDE_xTaskDelayUntil             1
#define INCLUDE_vTaskSuspend                1
#define INCLUDE_uxTaskGetStackHighWaterMark 1

#ifndef configASSERT
void board_exit(int code);
#define configASSERT(x) do { if (!(x)) board_exit(5); } while (0)
#endif

#endif
