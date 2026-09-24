/* OpenDoctrines, on TempleOS.
 *
 * ── WHAT THIS IS ──
 *
 * The game does not run here and will not: TempleOS has no C++ compiler, no
 * OpenGL and no network stack, and Open Doctrines needs all three. What DOES
 * run here is the other half of a real game. The engine's agent door
 * (`OpenDoctrinesServer --bench-agent`) prints a turn as plain text and reads
 * one line of `module:action` tokens back -- so a client only has to read a
 * file, draw 80x60 characters, and write a file. That is well inside what this
 * OS offers, and the turn it plays is a turn of the actual game.
 *
 * The host runs the engine. This runs the player.
 *
 * ── STATUS: NOT YET COMPILED ──
 *
 * This has never been through HolyC. It was written against the protocol and
 * against a rendering mockup, on a machine with no TempleOS on it. Treat every
 * line as a proposal until it has been through the compiler once; the README
 * lists what to check first and what is most likely wrong.
 *
 * ── THIS VERSION READS ONLY ──
 *
 * It parses a captured turn and draws it. It does not send orders yet, on
 * purpose: parsing and drawing is where everything uncertain lives, and a
 * client that can show you a real position is already the thing worth proving.
 * Writing a line of tokens back is the easy half and comes after.
 */

#define OD_MAX_ACTS  16     // per module; the door offers at most 12
#define OD_COLS      80

class ODAct
{
  U8 letter;                // 'e', 'p', 'w' or 'n'
  I64 num;                  // the action number the door printed
  U8 name[32];
};

class ODModule
{
  U8 label[12];
  U8 letter;
  I64 budget;
  I64 count;
  ODAct act[OD_MAX_ACTS];
};

class ODTurn
{
  I64 turn, turns;
  U8 country[32];
  U8 iso[8];
  I64 mine, owned;
  U8 share[12];             // kept as text: no F64 parsing, no rounding to argue about
  I64 army;
  U8 treasury[16];
  U8 gross[16], net[16];
  U8 war[64];
  ODModule mod[4];          // economy, politics, war, navy, in the door's order
  Bool waiting;             // the door reached "[AGENT] waiting"
};

/* ── little string helpers ──
 *
 * Written out rather than reached for because the parsing has to be forgiving:
 * the door pads with runs of spaces, and a client that assumes a fixed column
 * breaks the first time a country name gets longer. */

U8 *ODSkipSpace(U8 *p)
{
  while (*p == ' ') p++;
  return p;
}

/* Copy up to `max-1` bytes, stopping at `stop` or end of line. NUL-terminates. */
U0 ODCopyUntil(U8 *dst, I64 max, U8 *src, U8 stop)
{
  I64 i = 0;
  while (src[i] && src[i] != stop && src[i] != '\n' && i < max - 1)
  {
    dst[i] = src[i];
    i++;
  }
  dst[i] = 0;
}

/* Non-negative integer at p. Advances p past it. */
I64 ODNum(U8 **p)
{
  I64 v = 0;
  U8 *q = *p;
  while (*q >= '0' && *q <= '9')
  {
    v = v * 10 + (*q - '0');
    q++;
  }
  *p = q;
  return v;
}

/* Does `line` begin with `pre`? */
Bool ODStarts(U8 *line, U8 *pre)
{
  while (*pre)
  {
    if (*line != *pre) return FALSE;
    line++;
    pre++;
  }
  return TRUE;
}

/* ── the menus ──
 *
 * The door prints a module as one line, items joined by TWO spaces:
 *
 *   [AGENT] economy  e:0 save  e:1 industry  e:4 specialize
 *
 * Two, not one, because names contain single spaces ("declare war", "focus
 * bldg"). Splitting on one space merges two actions into one, silently. The
 * engine's own tests/agent_protocol_test.sh pins that separator so this parser
 * cannot be broken from the other side without something going red.
 */
U0 ODParseMenu(ODModule *m, U8 *rest)
{
  U8 *p = ODSkipSpace(rest);
  m->count = 0;
  while (*p && *p != '\n' && m->count < OD_MAX_ACTS)
  {
    if (!(p[1] == ':')) { p++; continue; }
    m->letter = p[0];
    p += 2;
    m->act[m->count].letter = m->letter;
    m->act[m->count].num = ODNum(&p);
    p = ODSkipSpace(p);
    /* The name runs to the next double space, or to the end of the line. */
    I64 i = 0;
    while (p[i] && p[i] != '\n' && i < 31)
    {
      if (p[i] == ' ' && p[i + 1] == ' ') break;
      i++;
    }
    MemCpy(m->act[m->count].name, p, i);
    m->act[m->count].name[i] = 0;
    p += i;
    p = ODSkipSpace(p);
    m->count++;
  }
}

/* ── one captured turn ──
 *
 * `buf` is the whole of what the door printed. Everything that is not an
 * [AGENT] line is ignored: the engine also logs [DIAG] and [AI] lines, and a
 * client that choked on those would break on any build with logging turned up.
 */
U0 ODParse(ODTurn *t, U8 *buf)
{
  MemSet(t, 0, sizeof(ODTurn));
  StrCpy(t->mod[0].label, "ECONOMY");  t->mod[0].letter = 'e';
  StrCpy(t->mod[1].label, "POLITICS"); t->mod[1].letter = 'p';
  StrCpy(t->mod[2].label, "WAR");      t->mod[2].letter = 'w';
  StrCpy(t->mod[3].label, "NAVY");     t->mod[3].letter = 'n';
  StrCpy(t->war, "(nobody)");

  U8 *p = buf;
  while (*p)
  {
    U8 *line = p;
    while (*p && *p != '\n') p++;
    if (*p) *p++ = 0;               /* cut the line out in place */

    if (!ODStarts(line, "[AGENT] ")) continue;
    U8 *a = line + 8;

    if (ODStarts(a, "===== turn "))
    {
      U8 *q = a + 11;
      t->turn = ODNum(&q);
      if (*q == '/') q++;
      t->turns = ODNum(&q);
      q = ODSkipSpace(q);
      ODCopyUntil(t->country, sizeof(t->country), q, '(');
      /* trim the space the door leaves before the bracket */
      I64 n = StrLen(t->country);
      while (n > 0 && t->country[n - 1] == ' ') { n--; t->country[n] = 0; }
      U8 *br = StrFirstOcc(q, "(");
      if (br) ODCopyUntil(t->iso, sizeof(t->iso), br + 1, ')');
    }
    else if (ODStarts(a, "land "))
    {
      U8 *q = a + 5;
      t->mine = ODNum(&q);
      if (*q == '/') q++;
      t->owned = ODNum(&q);
      U8 *br = StrFirstOcc(q, "(");
      if (br) ODCopyUntil(t->share, sizeof(t->share), br + 1, '%');
      U8 *ar = StrFirstOcc(q, "army");
      if (ar) { ar += 4; ar = ODSkipSpace(ar); t->army = ODNum(&ar); }
      U8 *tr = StrFirstOcc(q, "treasury");
      if (tr) { tr += 8; tr = ODSkipSpace(tr); ODCopyUntil(t->treasury, sizeof(t->treasury), tr, ' '); }
    }
    else if (ODStarts(a, "gross "))
    {
      U8 *q = a + 6;
      ODCopyUntil(t->gross, sizeof(t->gross), q, ' ');
      U8 *nt = StrFirstOcc(q, "net");
      if (nt) { nt += 3; nt = ODSkipSpace(nt); ODCopyUntil(t->net, sizeof(t->net), nt, ' '); }
    }
    else if (ODStarts(a, "at war with: "))
      ODCopyUntil(t->war, sizeof(t->war), a + 13, 0);
    else if (ODStarts(a, "economy"))  ODParseMenu(&t->mod[0], a + 7);
    else if (ODStarts(a, "politics")) ODParseMenu(&t->mod[1], a + 8);
    else if (ODStarts(a, "war "))     ODParseMenu(&t->mod[2], a + 4);
    else if (ODStarts(a, "navy"))     ODParseMenu(&t->mod[3], a + 4);
    else if (ODStarts(a, "budget "))
    {
      U8 *q = a + 7;
      I64 i;
      for (i = 0; i < 4; i++)
      {
        while (*q && *q != ':') q++;
        if (!*q) break;
        q++;
        t->mod[i].budget = ODNum(&q);
      }
    }
    else if (ODStarts(a, "waiting"))
      t->waiting = TRUE;
  }
}

/* ── drawing ──
 *
 * 80x60, sixteen colours, and the layout came from a mockup built on a real
 * capture rather than from imagination -- see docs/templeos.md. The one thing
 * the mockup settled: the door's menu lines reach 144 characters, so they are
 * re-laid out in two columns rather than echoed.
 */
U0 ODModuleAt(ODModule *m, I64 x, I64 y)
{
  I64 i;
  "$$CM,%d,%d$$$$FG,BLUE$$%s$$FG$$", x, y, m->label;
  "$$CM,%d,%d$$$$FG,DKGRAY$$budget %d$$FG$$", x + 20, y, m->budget;
  for (i = 0; i < m->count; i++)
  {
    "$$CM,%d,%d$$$$FG,CYAN$$%c:%d$$FG$$",
        x + 1, y + 1 + i, m->act[i].letter, m->act[i].num;
    "$$CM,%d,%d$$%s", x + 6, y + 1 + i, m->act[i].name;
  }
}

U0 ODDraw(ODTurn *t)
{
  I64 i, second;

  DocClear;
  "$$BG,BLUE$$$$FG,WHITE$$ OpenDoctrines  Agent Session";
  "$$CM,%d,0$$Turn %d/%d $$FG$$$$BG$$\n", OD_COLS - 18, t->turn, t->turns;

  "$$CM,1,2$$$$FG,RED$$%s (%s)$$FG$$", t->country, t->iso;
  "$$CM,20,2$$$$FG,DKGRAY$$Land$$FG$$  %d/%d  %s%%", t->mine, t->owned, t->share;
  "$$CM,46,2$$$$FG,DKGRAY$$Army$$FG$$  %d", t->army;
  "$$CM,20,3$$$$FG,DKGRAY$$Treas$$FG$$ %s", t->treasury;
  "$$CM,46,3$$$$FG,DKGRAY$$Net$$FG$$   %s  (gross %s)", t->net, t->gross;
  "$$CM,1,4$$$$FG,DKGRAY$$At war with:$$FG$$ %s", t->war;

  /* Two modules across, two down. The second row starts under whichever of the
   * first pair is taller, so a long economy menu never writes over politics. */
  ODModuleAt(&t->mod[0], 1, 7);      /* economy */
  ODModuleAt(&t->mod[2], 41, 7);     /* war */
  second = 7 + t->mod[0].count;
  if (t->mod[2].count > t->mod[0].count) second = 7 + t->mod[2].count;
  second += 3;
  ODModuleAt(&t->mod[1], 1, second);   /* politics */
  ODModuleAt(&t->mod[3], 41, second);  /* navy */

  if (!t->waiting)
    "$$CM,1,56$$$$FG,RED$$This capture never reached '[AGENT] waiting'.$$FG$$";
  "$$CM,1,58$$$$FG,DKGRAY$$Read-only. Orders are not sent yet.$$FG$$\n";
}

/* ── the entry point ──
 *
 * Takes the path of a captured turn. The bridge that puts one there, and takes
 * an order line back, is the host's job and is described in the README -- this
 * OS has no network, so the file IS the network. */
U0 OpenDoc(U8 *path = "C:/Home/turn.txt")
{
  I64 size = 0;
  U8 *buf = FileRead(path, &size);
  if (!buf)
  {
    "$$FG,RED$$Cannot read %s$$FG$$\n", path;
    return;
  }
  ODTurn t;
  ODParse(&t, buf);
  Free(buf);
  ODDraw(&t);
}
