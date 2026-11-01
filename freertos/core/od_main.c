/* The game under FreeRTOS: three tasks and nothing shared without a handshake.
 *
 *   console  reads the serial line and runs the shell. Everything printed is
 *            printed from here, so no output needs a lock.
 *   engine   resolves a turn when the console asks, and says when it is done.
 *            A turn moves every army in the world, so it runs at a lower
 *            priority than anything that has to answer on time -- on a real
 *            spacecraft that is the attitude loop, here it is the beacon.
 *   beacon   the heartbeat a satellite sends whatever else it is doing. It
 *            counts, once a second, and "hk" shows that it kept counting
 *            while a turn was being resolved.
 *
 * The console waits while the engine runs, so the world is only ever touched
 * by one task at a time, and the same commands produce the same game on every
 * board: tests/freertos_test.sh holds all of them to one transcript.
 */
#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "od_rules.h"
#include "od_shell.h"
#include "od_world.h"

static TaskHandle_t console_task, engine_task, beacon_task;
static volatile uint32_t beacons;
static TickType_t last_turn_ticks;

static int con_getch(void)
{
    for (;;) {
        int c = board_getc();
        if (c == BOARD_EOF) return -1;
        if (c != BOARD_NONE) return c;
        vTaskDelay(1);
    }
}

static void con_write(const char *s, size_t n)
{
    while (n--) board_putc(*s++);
}

static void con_end_turn(void)
{
    xTaskNotifyGive(engine_task);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

static void con_housekeeping(void)
{
    static TaskStatus_t st[8];
    UBaseType_t n = uxTaskGetSystemState(st, 8, NULL);
    od_printf("uptime %lu ms, beacon %lu, last turn %lu ms\n",
              (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS),
              (unsigned long)beacons, (unsigned long)(last_turn_ticks * portTICK_PERIOD_MS));
    od_printf("heap free %lu of %lu bytes, world state %lu bytes\n",
              (unsigned long)xPortGetFreeHeapSize(), (unsigned long)configTOTAL_HEAP_SIZE,
              (unsigned long)sizeof od_w);
    for (UBaseType_t i = 0; i < n; i++)
        od_printf("  %-8s priority %lu, %lu words of stack never used\n", st[i].pcTaskName,
                  (unsigned long)st[i].uxCurrentPriority, (unsigned long)st[i].usStackHighWaterMark);
}

static void console(void *arg)
{
    (void)arg;
    static od_io_t io = {con_getch, con_write, con_end_turn, con_housekeeping, board_name, 0};
    io.echo = board_echo;
    od_shell_run(&io);
    board_exit(0);
    vTaskSuspend(NULL);
}

static void engine(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        TickType_t t0 = xTaskGetTickCount();
        od_turn();
        last_turn_ticks = xTaskGetTickCount() - t0;
        xTaskNotifyGive(console_task);
    }
}

static void beacon(void *arg)
{
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(1000));
        beacons++;
    }
}

int main(void)
{
    board_init();
    od_world_reset(0);
    xTaskCreate(beacon, "beacon", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 3, &beacon_task);
    xTaskCreate(console, "console", OD_CONSOLE_STACK, NULL, tskIDLE_PRIORITY + 2, &console_task);
    xTaskCreate(engine, "engine", OD_ENGINE_STACK, NULL, tskIDLE_PRIORITY + 1, &engine_task);
    vTaskStartScheduler();
    for (;;) {
    }
}

void vApplicationStackOverflowHook(TaskHandle_t t, char *name)
{
    (void)t;
    const char *m = "\r\nstack overflow in ";
    while (*m) board_putc(*m++);
    while (*name) board_putc(*name++);
    board_exit(3);
    for (;;) {
    }
}

void vApplicationMallocFailedHook(void)
{
    const char *m = "\r\nout of heap\r\n";
    while (*m) board_putc(*m++);
    board_exit(4);
    for (;;) {
    }
}
