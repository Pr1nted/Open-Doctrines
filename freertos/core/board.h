/* What a board gives the game: a serial line, a way to stop, and its name.
 *
 * Kept to the least a board can be asked for, so a new one is a page of code:
 * freertos/ports/<board>/board.c and a linker script, and nothing else. The
 * console polls rather than taking an interrupt -- one character every
 * millisecond is 10,000 baud, more than anybody types -- which keeps every
 * port free of interrupt priorities, the part of a FreeRTOS port most easily
 * got wrong.
 */
#ifndef OD_BOARD_H
#define OD_BOARD_H

#define BOARD_NONE  (-1)       /* nothing waiting */
#define BOARD_EOF   (-2)       /* input has ended: the host build's stdin */

extern const char board_name[];
extern const int  board_echo;    /* 1 on a serial line; 0 where a terminal echoes */
void board_init(void);
int  board_getc(void);         /* never blocks */
void board_putc(char c);
void board_exit(int code);     /* "quit": QEMU exits; real hardware idles */

#endif
