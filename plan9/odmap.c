/* Open Doctrines on Plan 9: the map.
 *
 * The same game as the console (opendoctrines.c) in a rio window drawn with
 * libdraw: every province a square at its centre on the world raster, as big
 * as its area, in its owner's colour. Click a province to select it (the
 * console's "show" for it is printed below the map); type the console's
 * commands at the bottom line and press Enter. Del, or "quit", leaves.
 *
 *   odmap [SEED]
 *
 * Yours are drawn white-edged; a selected province is ringed in red and its
 * neighbours in yellow. Button 3 on a province of yours types its number into
 * the command line, for "recruit", "attack" and "bombard".
 */
#include "od9.h"
#include <draw.h>
#include <event.h>
#include "od_map.h"
#include "od_rules.h"
#include "od_shell.h"
#include "od_world.h"

enum {
	Nlog = 64,		/* lines kept below the map */
	Loglen = 128,
};

static Image *ctry[OD_MAXC], *sea, *grey, *red, *yellow, *white, *dark;
static char logl[Nlog][Loglen];
static int nlog;		/* lines in logl, newest last */
static char part[Loglen];	/* the line being written */
static int npart;
static char cmd[96];
static int ncmd;
static int sel = -1;
static Rectangle maprect;

static void
logline(char *s)
{
	if(nlog == Nlog){
		memmove(logl[0], logl[1], (Nlog-1)*Loglen);
		nlog--;
	}
	strncpy(logl[nlog], s, Loglen-1);
	logl[nlog][Loglen-1] = 0;
	nlog++;
}

static void
mapwrite(const char *s, size_t n)
{
	size_t i;

	for(i = 0; i < n; i++){
		if(s[i] == '\n' || npart == Loglen-1){
			part[npart] = 0;
			logline(part);
			npart = 0;
			if(s[i] == '\n')
				continue;
		}
		part[npart++] = s[i];
	}
}

static void
hk(void)
{
	od_printf("  world state %lu bytes; screen %d x %d\n", (ulong)sizeof od_w,
		Dx(screen->r), Dy(screen->r));
}

/* A colour per country that stays put from game to game: a hash of its slot. */
static ulong
ctrycolour(int c)
{
	ulong h = (ulong)c * 2654435761UL;
	int r = 64 + (h >> 8 & 0x9F), g = 64 + (h >> 16 & 0x9F), b = 64 + (h >> 24 & 0x9F);
	return (ulong)r<<24 | (ulong)g<<16 | (ulong)b<<8 | 0xFF;
}

static Image*
colour(ulong rgba)
{
	Image *i;

	i = allocimage(display, Rect(0, 0, 1, 1), screen->chan, 1, rgba);
	if(i == nil)
		sysfatal("allocimage: %r");
	return i;
}

static Point
centre(int i)
{
	Point p;

	p.x = maprect.min.x + (vlong)od_cx[i] * Dx(maprect) / od_map_w;
	p.y = maprect.min.y + (vlong)od_cy[i] * Dy(maprect) / od_map_h;
	return p;
}

/* Half the side of a province's square, from its area on the original raster. */
static int
half(int i)
{
	double side;
	int h;

	side = sqrt((double)od_prov0[i].area) * Dx(maprect) / 4096.0;
	h = side / 2;
	if(h < 1)
		h = 1;
	if(h > 8)
		h = 8;
	return h;
}

static Rectangle
square(int i)
{
	Point c;
	int h;

	c = centre(i);
	h = half(i);
	return Rect(c.x-h, c.y-h, c.x+h+1, c.y+h+1);
}

static void
redraw(void)
{
	Rectangle r;
	Point p;
	int i, k, lines, first;
	char status[128];

	draw(screen, screen->r, dark, nil, ZP);
	r = screen->r;
	lines = (Dy(r) / 3) / font->height;
	if(lines < 3)
		lines = 3;
	maprect = Rect(r.min.x+4, r.min.y+4, r.max.x-4, r.max.y - (lines+2)*font->height - 8);
	/* keep the raster's 2:1 */
	if(Dx(maprect) > 2*Dy(maprect))
		maprect.max.x = maprect.min.x + 2*Dy(maprect);
	else
		maprect.max.y = maprect.min.y + Dx(maprect)/2;
	draw(screen, maprect, sea, nil, ZP);

	/* the large first, so the small stay visible on top of them */
	for(k = 8; k >= 1; k--)
		for(i = 0; i < od_np; i++){
			if(half(i) != k)
				continue;
			draw(screen, square(i), od_w.own[i] < od_nc ? ctry[od_w.own[i]] : grey, nil, ZP);
			if(od_w.own[i] == od_w.me && od_w.me < od_nc && k > 1)
				border(screen, square(i), 1, white, ZP);
		}
	if(sel >= 0){
		for(k = 0; k < od_prov0[sel].adjn; k++)
			border(screen, insetrect(square(od_neighbour(sel, k)), -1), 1, yellow, ZP);
		border(screen, insetrect(square(sel), -2), 2, red, ZP);
	}

	p = Pt(r.min.x+4, maprect.max.y+4);
	if(od_w.me < od_nc)
		snprint(status, sizeof status, "%s (%s), turn %d%s%s", od_ctry0[od_w.me].name,
			od_ctry0[od_w.me].iso, od_w.turn, sel >= 0 ? " -- selected: " : "",
			sel >= 0 ? od_prov0[sel].name : "");
	else
		snprint(status, sizeof status, "turn %d -- \"play SWE\" to govern a country; click a province", od_w.turn);
	string(screen, p, white, ZP, font, status);
	p.y += font->height;
	first = nlog - lines;
	if(first < 0)
		first = 0;
	for(i = first; i < nlog; i++){
		string(screen, p, display->white, ZP, font, logl[i]);
		p.y += font->height;
	}
	p.y = r.max.y - font->height - 4;
	p = string(screen, p, yellow, ZP, font, "> ");
	cmd[ncmd] = 0;
	p = string(screen, p, white, ZP, font, cmd);
	draw(screen, Rect(p.x, p.y, p.x+2, p.y+font->height), white, nil, ZP);
	flushimage(display, 1);
}

void
eresized(int new)
{
	if(new && getwindow(display, Refnone) < 0)
		sysfatal("getwindow: %r");
	redraw();
}

static int
nearest(Point m)
{
	int i, best;
	vlong d, bd;
	Point c;

	best = -1;
	bd = 0;
	for(i = 0; i < od_np; i++){
		c = centre(i);
		d = (vlong)(c.x-m.x)*(c.x-m.x) + (vlong)(c.y-m.y)*(c.y-m.y);
		if(best < 0 || d < bd){
			best = i;
			bd = d;
		}
	}
	return bd <= 20*20 ? best : -1;
}

static void
run(char *line)
{
	char copy[96];

	od_printf("> %s\n", line);
	strncpy(copy, line, sizeof copy - 1);
	copy[sizeof copy - 1] = 0;
	if(!od_shell_line(copy))
		exits(nil);
}

void
main(int argc, char **argv)
{
	od_io_t io;
	Event e;
	int c, lastb;
	char buf[32];

	od_world_reset(argc > 1 ? (uint32_t)strtoul(argv[1], nil, 10) : 0);
	io.getch = nil;
	io.write = mapwrite;
	io.end_turn = od_turn;
	io.housekeeping = hk;
	io.board = "libdraw";
	io.echo = 0;
	od_edition = "Plan 9";
	od_shell_attach(&io);

	if(initdraw(nil, nil, "opendoctrines") < 0)
		sysfatal("initdraw: %r");
	for(c = 0; c < od_nc && c < OD_MAXC; c++)
		ctry[c] = colour(ctrycolour(c));
	sea = colour(0x1B3550FF);
	grey = colour(0x707070FF);
	red = colour(DRed);
	yellow = colour(DYellow);
	white = colour(DWhite);
	dark = colour(0x101820FF);
	einit(Emouse|Ekeyboard);

	od_printf("Open Doctrines on Plan 9: %d provinces, %d countries. \"help\" for commands.\n",
		od_np, od_nc);
	redraw();
	lastb = 0;
	for(;;){
		switch(event(&e)){
		case Emouse:
			if(e.mouse.buttons && !lastb && ptinrect(e.mouse.xy, maprect)){
				sel = nearest(e.mouse.xy);
				if(sel >= 0 && (e.mouse.buttons & 4) && ncmd + 6 < sizeof cmd - 1){
					snprint(buf, sizeof buf, "%s%d", ncmd && cmd[ncmd-1] != ' ' ? " " : "", sel);
					strcpy(cmd+ncmd, buf);
					ncmd += strlen(buf);
				}else if(sel >= 0){
					snprint(buf, sizeof buf, "show %d", sel);
					run(buf);
				}
				redraw();
			}
			lastb = e.mouse.buttons;
			break;
		case Ekeyboard:
			c = e.kbdc;
			if(c == 0x7F)		/* Del: Kdel, without <keyboard.h> */
				exits(nil);
			if(c == '\n' || c == '\r'){
				cmd[ncmd] = 0;
				if(ncmd)
					run(cmd);
				ncmd = 0;
			}else if(c == '\b'){
				if(ncmd > 0)
					ncmd--;
			}else if(c >= ' ' && c < 0x7F && ncmd < sizeof cmd - 1)
				cmd[ncmd++] = c;
			redraw();
			break;
		}
	}
}
