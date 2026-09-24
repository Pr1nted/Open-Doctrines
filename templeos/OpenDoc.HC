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

/* ── FIELD SIZES ARE NAMED, BECAUSE sizeof CANNOT ASK ──
 *
 * HolyC's sizeof takes a TYPE. `OD_COUNTRY_SZ` is not a size, it is a
 * parse error -- "Missing ')'", pointing at a line whose brackets are all
 * balanced, which is a thoroughly misleading thing to be told. So each buffer's
 * length is a name used BOTH in the class and at every call, and the two cannot
 * drift apart. */
#define OD_COUNTRY_SZ  32
#define OD_ISO_SZ       8
#define OD_SHARE_SZ    12
#define OD_MONEY_SZ    16
#define OD_WAR_SZ      64

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
  U8 country[OD_COUNTRY_SZ];
  U8 iso[OD_ISO_SZ];
  I64 mine, owned;
  U8 share[OD_SHARE_SZ];             // kept as text: no F64 parsing, no rounding to argue about
  I64 army;
  U8 treasury[OD_MONEY_SZ];
  U8 gross[OD_MONEY_SZ], net[OD_MONEY_SZ];
  U8 war[OD_WAR_SZ];
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
    /* HolyC has no `continue` -- the compiler says so: "Undefined identifier
     * at ';'". So the skip is an else rather than an early jump. */
    if (p[1] != ':')
      p++;
    else
    {
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

  /* ── EVERY LOCAL, DECLARED ONCE, AT THE TOP ──
   *
   * HolyC has no block scope: a declaration anywhere in a function belongs to
   * the whole function, so a second `U8 *q` in the next branch is not a new
   * variable, it is "Duplicate member" and the file does not compile. Hoisting
   * them is not style -- it is the only way to write this. */
  U8 *p, *line, *a, *q, *br, *ar, *tr, *nt;
  I64 n, i;

  p = buf;
  while (*p)
  {
    line = p;
    while (*p && *p != '\n') p++;
    if (*p) *p++ = 0;               /* cut the line out in place */

    /* Same reason: no `continue`, so the whole body is inside the test. */
    if (ODStarts(line, "[AGENT] "))
    {
    a = line + 8;

    if (ODStarts(a, "===== turn "))
    {
      q = a + 11;
      t->turn = ODNum(&q);
      if (*q == '/') q++;
      t->turns = ODNum(&q);
      q = ODSkipSpace(q);
      ODCopyUntil(t->country, OD_COUNTRY_SZ, q, '(');
      /* trim the space the door leaves before the bracket */
      n = StrLen(t->country);
      while (n > 0 && t->country[n - 1] == ' ') { n--; t->country[n] = 0; }
      /* One character, so a set of one: this IS what StrFirstOcc is for.
       * The word searches above are StrMatch, because StrFirstOcc("army")
       * asks for the first a, r, m OR y -- which is the 'r' of "world", and
       * which is why this client first reported Sweden with no army and a
       * treasury of "(d)". */
      br = StrFirstOcc(q, "(");
      if (br) ODCopyUntil(t->iso, OD_ISO_SZ, br + 1, ')');
    }
    else if (ODStarts(a, "land "))
    {
      q = a + 5;
      t->mine = ODNum(&q);
      if (*q == '/') q++;
      t->owned = ODNum(&q);
      br = StrFirstOcc(q, "(");
      if (br) ODCopyUntil(t->share, OD_SHARE_SZ, br + 1, '%');
      ar = StrMatch("army", q);
      if (ar) { ar += 4; ar = ODSkipSpace(ar); t->army = ODNum(&ar); }
      tr = StrMatch("treasury", q);
      if (tr) { tr += 8; tr = ODSkipSpace(tr); ODCopyUntil(t->treasury, OD_MONEY_SZ, tr, ' '); }
    }
    else if (ODStarts(a, "gross "))
    {
      q = a + 6;
      ODCopyUntil(t->gross, OD_MONEY_SZ, q, ' ');
      nt = StrMatch("net", q);
      if (nt) { nt += 3; nt = ODSkipSpace(nt); ODCopyUntil(t->net, OD_MONEY_SZ, nt, ' '); }
    }
    else if (ODStarts(a, "at war with: "))
      ODCopyUntil(t->war, OD_WAR_SZ, a + 13, 0);
    else if (ODStarts(a, "economy"))  ODParseMenu(&t->mod[0], a + 7);
    else if (ODStarts(a, "politics")) ODParseMenu(&t->mod[1], a + 8);
    else if (ODStarts(a, "war "))     ODParseMenu(&t->mod[2], a + 4);
    else if (ODStarts(a, "navy"))     ODParseMenu(&t->mod[3], a + 4);
    else if (ODStarts(a, "budget "))
    {
      q = a + 7;
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
}

/* ── drawing ──
 *
 * 80x60 and sixteen colours, and the layout came from a mockup built on a real
 * capture rather than from imagination -- see docs/templeos.md. The one thing
 * the mockup settled: the door's menu lines reach 144 characters, so they are
 * re-laid out in two columns rather than echoed.
 *
 * ── WHOLE LINES, NOT CURSOR MOVES ──
 *
 * The first version placed every field with `$$CM,x,y$$`. On a real machine it
 * drew almost nothing: the window is a scrolling document, not a grid you can
 * address, and absolute positions in it do not mean what they look like they
 * mean. So the screen is built the way a printer builds one -- each row is
 * composed into a buffer, padded, and printed in order. Nothing moves; the
 * column is a count of spaces. It survives being scrolled, resized and piped.
 */
#define OD_MID       40     /* where the right-hand column starts, when there is one */
#define OD_NEED_WIDE 70     /* below this, one column */

/* ── FIT THE WINDOW YOU ARE GIVEN ──
 *
 * The desktop puts a task in the left half of the screen and its own help in
 * the right, about 40 columns each. An 80-column layout there does not just
 * look wrong -- every row folds in two, so the war menu lands underneath the
 * economy menu interleaved line for line, and the result is unreadable.
 *
 * Taking the whole width with WinHorz is worse, not better: the width is
 * granted, but the help task goes on painting its own window over the right
 * of the screen, and the last thirty columns of this display are quietly eaten
 * by somebody else's border. Measuring and laying out to fit needs nothing
 * from any other task, and widens by itself if the window is ever maximised. */
I64 od_width = OD_COLS;
Bool od_wide = TRUE;

/* Print two strings as one row, the second starting at column OD_MID.
 * The padding is measured on the text itself, so no colour codes may be in
 * these -- a "$$FG,RED$$" is eight characters to StrLen and none on screen,
 * and mixing the two is how columns come out ragged. Colour goes on the row. */
U0 ODRow(U8 *left, U8 *right, U8 *colour = NULL)
{
  U8 line[OD_COLS * 2];
  I64 n;

  if (!od_wide)
  {
    /* Narrow: the pair becomes two rows. Nothing is dropped and nothing wraps. */
    if (*left || !right || !*right)
    {
      if (colour) "$$FG,%s$$%s$$FG$$\n", colour, left;
      else        "%s\n", left;
    }
    if (right && *right)
    {
      if (colour) "$$FG,%s$$%s$$FG$$\n", colour, right;
      else        "%s\n", right;
    }
    return;
  }

  StrCpy(line, left);
  n = StrLen(line);
  if (right && *right)
  {
    while (n < OD_MID) line[n++] = ' ';
    line[n] = 0;
    StrCpy(line + n, right);
  }
  if (colour)
    "$$FG,%s$$%s$$FG$$\n", colour, line;
  else
    "%s\n", line;
}

/* Row `k` of a module: 0 is its heading, 1..count are its actions, and past
 * that an empty string -- so a short menu can sit beside a long one without
 * either of them needing to know the other's length. */
U0 ODModLine(ODModule *m, I64 k, U8 *out)
{
  *out = 0;
  if (!m->count && k) return;
  if (!k)
    StrPrint(out, "%s  (%c)  budget %d", m->label, m->letter, m->budget);
  else if (k <= m->count && m->act[k - 1].num < 10)
    StrPrint(out, "  %c:%d  %s", m->act[k - 1].letter, m->act[k - 1].num,
             m->act[k - 1].name);
  else if (k <= m->count)   /* the width flag in "%-2d" is ignored here, so
                             * the single digits are padded by hand instead */
    StrPrint(out, "  %c:%d %s", m->act[k - 1].letter, m->act[k - 1].num,
             m->act[k - 1].name);
}

/* One module, stacked -- the narrow layout. */
U0 ODModule1(ODModule *m)
{
  U8 l[96];
  I64 k;

  ODModLine(m, 0, l);
  ODRow(l, "", "BLUE");
  for (k = 1; k <= m->count; k++)
  {
    ODModLine(m, k, l);
    ODRow(l, "");
  }
  ODRow("", "");
}

/* Two modules side by side, as tall as the taller one -- or one after the
 * other when the window cannot hold two. Interleaving them row by row in a
 * narrow window is exactly the unreadable thing this avoids. */
U0 ODPair(ODModule *a, ODModule *b)
{
  U8 l[96], r[96];
  I64 k, rows;

  if (!od_wide)
  {
    ODModule1(a);
    ODModule1(b);
    return;
  }

  rows = a->count;
  if (b->count > rows) rows = b->count;
  ODModLine(a, 0, l); ODModLine(b, 0, r);
  ODRow(l, r, "BLUE");
  for (k = 1; k <= rows; k++)
  {
    ODModLine(a, k, l);
    ODModLine(b, k, r);
    ODRow(l, r);
  }
  ODRow("", "");
}

U0 ODDraw(ODTurn *t)
{
  U8 l[96], r[96];

  od_width = Fs->win_right - Fs->win_left + 1;
  if (od_width < 20) od_width = OD_COLS;   /* not a sane answer; assume a screen */
  od_wide = od_width >= OD_NEED_WIDE;

  DocClear;
  StrPrint(l, " OpenDoctrines  Agent Session");
  StrPrint(r, "Turn %d/%d", t->turn, t->turns);
  ODRow(l, r, "BLUE");
  ODRow("", "");

  StrPrint(l, "%s (%s)", t->country, t->iso);
  StrPrint(r, "Land %d/%d (%s%%)   Army %d", t->mine, t->owned, t->share, t->army);
  ODRow(l, r, "RED");

  StrPrint(l, "Treasury %s", t->treasury);
  StrPrint(r, "Net %s   (gross %s)", t->net, t->gross);
  ODRow(l, r);

  StrPrint(l, "At war with: %s", t->war);
  ODRow(l, "");
  ODRow("", "");

  ODPair(&t->mod[0], &t->mod[2]);   /* economy | war   */
  ODPair(&t->mod[1], &t->mod[3]);   /* politics | navy */

  if (!t->waiting)
    ODRow("This capture never reached '[AGENT] waiting'.", "", "RED");
}

/* ── the entry point ──
 *
 * Takes the path of a captured turn, relative to wherever you are: the drive
 * letter is deliberately not baked in, because which one this lands on depends
 * on how the machine was installed -- mine boots D:, a C: install is just as
 * ordinary, and a client that insists on one of them is broken on the other.
 *
 * The bridge that puts a turn there and takes an order line back is the host's
 * job and is described in the README -- this OS has no network, so the file IS
 * the network. */
/* ── READING A FILE WHOSE NAME MAY HAVE BEEN SHOUTED AT ──
 *
 * A name that fits 8.3 and is all lower case -- `turn.txt` -- is stored by
 * other systems as a short entry with a "this was lower case" flag, and this
 * OS's FAT driver does not read that flag. So the file the host wrote as
 * turn.txt is TURN.TXT here, and opening it by the name you wrote fails.
 * `OpenDoc.HC` is unaffected only because mixed case forces a long-name entry.
 *
 * Rather than making everybody remember which of their filenames happens to be
 * 8.3-shaped, try the name as given and then the shouted version of the last
 * component. The directory part is left alone: `Home` is itself a long-name
 * entry and HOME would not find it. */
U8 *ODReadEither(U8 *path, I64 *size)
{
  U8 *buf, *slash, upper[64];
  I64 i, cut;

  buf = FileRead(path, size);
  if (buf) return buf;

  slash = StrLastOcc(path, "/");
  cut = 0;
  if (slash) cut = slash - path + 1;
  if (StrLen(path) >= sizeof(upper)) return NULL;
  StrCpy(upper, path);
  for (i = cut; upper[i]; i++)
    if (upper[i] >= 'a' && upper[i] <= 'z') upper[i] -= 32;
  if (!StrCmp(upper, path)) return NULL;
  return FileRead(upper, size);
}

/* ── SENDING ORDERS BACK ──
 *
 * One line of `module:action` tokens, the same line the door reads from its
 * FIFO on the host. It is written with the turn it answers in the first line:
 *
 *     # turn 5
 *     e:1, w:1
 *
 * which is the whole defence against a leftover file. A file from turn 4 looks
 * exactly like a file for turn 5, and if the host took it the player would
 * quietly stop being asked and the game would play itself. The host refuses
 * anything whose number is not the turn on offer -- see templeos/bridge.py,
 * where the same rule is tested.
 */

/* The module a letter names, or NULL. */
ODModule *ODModuleOf(ODTurn *t, U8 letter)
{
  I64 i;
  for (i = 0; i < 4; i++)
    if (t->mod[i].letter == letter) return &t->mod[i];
  return NULL;
}

/* ── CHECK THE ORDER AGAINST THE MENU THAT WAS OFFERED ──
 *
 * The engine drops a token it does not recognise without saying so, which from
 * this side is indistinguishable from an order that was carried out. A typo
 * would read as the game ignoring you. So a number that is not on the menu is
 * refused here, where it can still be retyped. */
Bool ODCheck(ODTurn *t, U8 *line)
{
  ODModule *m;
  U8 *q;
  I64 n, i;
  Bool found;

  q = line;
  while (*q)
  {
    while (*q == ' ' || *q == ',') q++;
    if (*q)
    {
      m = ODModuleOf(t, *q);
      if (!m)
      {
        "$$FG,RED$$'%c' is not one of e, p, w, n.$$FG$$\n", *q;
        return FALSE;
      }
      q++;
      if (*q != ':')
      {
        "$$FG,RED$$Expected a ':' after '%c'.$$FG$$\n", m->letter;
        return FALSE;
      }
      q++;
      n = ODNum(&q);
      found = FALSE;
      for (i = 0; i < m->count; i++)
        if (m->act[i].num == n) found = TRUE;
      if (!found)
      {
        "$$FG,RED$$%s has no action %d this turn.$$FG$$\n", m->label, n;
        return FALSE;
      }
    }
  }
  return TRUE;
}

U0 ODWriteOrders(ODTurn *t, U8 *line, U8 *path)
{
  U8 out[512];
  StrPrint(out, "# turn %d\n%s\n", t->turn, line);
  FileWrite(path, out, StrLen(out));
  "$$FG,GREEN$$Sent for turn %d: %s$$FG$$\n", t->turn, line;
}

U0 OpenDoc(U8 *path = "turn.txt", U8 *orders = "orders.txt")
{
  I64 size = 0;
  U8 *buf = ODReadEither(path, &size);
  U8 *line;
  ODTurn t;

  if (!buf)
  {
    "$$FG,RED$$Cannot read %s (tried it shouted, too)$$FG$$\n", path;
    return;
  }
  ODParse(&t, buf);
  Free(buf);
  ODDraw(&t);

  if (!t.waiting)
  {
    ODRow("This turn never reached '[AGENT] waiting' -- nothing to answer.",
          "", "RED");
    return;
  }

  /* Empty is a legitimate answer: it means hold everything. Somebody who wants
   * to look at the position and decide later just presses escape. */
  line = GetStr("orders (e:1, w:1 ...  empty = hold): ");
  if (!line)
    return;
  if (ODCheck(&t, line))
    ODWriteOrders(&t, line, orders);
  Free(line);
}
