/* Open Doctrines on Plan 9: the console.
 *
 * The FreeRTOS edition's core -- the same rules, the same world, the same
 * shell -- with Plan 9's file descriptors as its serial line: commands from
 * fd 0, answers to fd 1. So it runs in a rio window, over a serial console,
 * or with a script on its standard input:
 *
 *   opendoctrines [SEED]
 *   opendoctrines < play.txt
 *
 * A rio window and the kernel's console both echo what is typed, so the
 * shell does not.
 */
#include "od9.h"
#include "od_rules.h"
#include "od_shell.h"
#include "od_world.h"

static uchar inbuf[512];
static long  inlen, inpos;

static int
p9getch(void)
{
	if(inpos >= inlen){
		inlen = read(0, inbuf, sizeof inbuf);
		inpos = 0;
		if(inlen <= 0)
			return -1;
	}
	return inbuf[inpos++];
}

static void
p9write(const char *s, size_t n)
{
	write(1, s, n);
}

static void
p9hk(void)
{
	od_printf("  world state %lu bytes (od_w); the world itself is in the text segment\n",
		(ulong)sizeof od_w);
	od_printf("  %d provinces, %d countries, %d hulls; no scheduler, one process\n",
		od_np, od_nc, od_ns);
}

void
main(int argc, char **argv)
{
	od_io_t io;

	od_world_reset(argc > 1 ? (uint32_t)strtoul(argv[1], nil, 10) : 0);
	io.getch = p9getch;
	io.write = p9write;
	io.end_turn = od_turn;
	io.housekeeping = p9hk;
	io.board = "console";
	io.echo = 0;
	od_edition = "Plan 9";
	od_shell_run(&io);
	exits(nil);
}
