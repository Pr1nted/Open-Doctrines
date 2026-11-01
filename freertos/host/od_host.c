/* The core with no scheduler: stdin in, stdout out, the turn run in place.
 *
 * What tests/freertos_test.sh plays, because a transcript from here and one
 * from a board under QEMU must agree line for line: the rules do not know
 * which they are running on, and this is how that is checked.
 */
#include "od_rules.h"
#include "od_shell.h"
#include "od_world.h"

#include <stdio.h>
#include <stdlib.h>

static int host_getch(void)
{
    fflush(stdout);         /* the prompt, before waiting on a pipe */
    return getchar();
}
static void host_write(const char *s, size_t n) { fwrite(s, 1, n, stdout); }

int main(int argc, char **argv)
{
    od_world_reset(argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 0);
    od_io_t io = {host_getch, host_write, od_turn, NULL, "host (no scheduler)", 0};
    od_shell_run(&io);
    return 0;
}
