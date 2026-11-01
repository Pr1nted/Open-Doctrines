/* The console: a line at a time over a serial port.
 *
 * A CubeSat's on-board computer has no screen and no keyboard; what it has is
 * a UART, and a ground station types at it. So this is the game's whole user
 * interface -- the same orders the TempleOS edition gives with a mouse, given
 * as commands, and answered in plain text that fits an 80-column terminal.
 *
 * Nothing in here knows about FreeRTOS. The board glue hands over three
 * functions -- read a character, write some bytes, and resolve a turn -- so
 * the same shell runs under the scheduler on every board, and in the host
 * test with no scheduler at all.
 */
#ifndef OD_SHELL_H
#define OD_SHELL_H

#include <stddef.h>

typedef struct {
    int  (*getch)(void);                     /* blocks; -1 at end of input */
    void (*write)(const char *s, size_t n);
    void (*end_turn)(void);                  /* runs od_turn, wherever it runs */
    void (*housekeeping)(void);              /* the "hk" command; may be NULL */
    const char *board;                       /* shown in the banner */
    int  echo;                               /* echo what is typed: a serial line does not */
} od_io_t;

extern const char *od_edition;                /* "FreeRTOS", unless a port says otherwise */
void od_printf(const char *fmt, ...);
void od_shell_run(const od_io_t *io);        /* returns at end of input or "quit" */
void od_shell_attach(const od_io_t *io);      /* od_printf's output, without the loop */
int  od_shell_line(char *line);              /* one command; 0 when it asked to quit */

#endif
