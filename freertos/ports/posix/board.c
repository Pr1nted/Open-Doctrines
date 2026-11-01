/* The FreeRTOS kernel on Linux or macOS, through its POSIX port: the real
 * scheduler, the real tasks, stdin and stdout for the serial line. The quick
 * way to play the FreeRTOS build, and how CI tries it before any emulator. */
#include "board.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

const char board_name[] = "POSIX (FreeRTOS kernel on the host)";
const int board_echo = 0;

void board_init(void)
{
}

int board_getc(void)
{
    struct pollfd p = {0, POLLIN, 0};
    int r = poll(&p, 1, 0);
    if (r < 0) return errno == EINTR ? BOARD_NONE : BOARD_EOF;
    if (r == 0) return BOARD_NONE;
    unsigned char c;
    ssize_t n = read(0, &c, 1);
    if (n == 1) return c;
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return BOARD_NONE;
    return BOARD_EOF;
}

void board_putc(char c)
{
    /* A terminal does its own carriage returns. */
    if (c == '\r') return;
    putchar(c);
    if (c == '\n' || c == ' ') fflush(stdout);
}

void board_exit(int code)
{
    fflush(stdout);
    exit(code);
}
