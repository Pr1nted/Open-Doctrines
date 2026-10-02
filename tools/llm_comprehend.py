#!/usr/bin/env python3
"""Does the LLM advisor actually UNDERSTAND this game, and how well?

The advisor module has been judged until now by whether its letters read well.
That is not comprehension. A model can write a fluent despatch about a war it
has the wrong side of, and the game would not notice: `tools/check_llm_tools.py`
proves every tool it is offered has an answer, which is wiring, and journal 436
found the entire our_* family returning "There is no country by that name in
this world" while every letter still read plausibly.

So this scores comprehension directly, against ground truth taken from the
code and from a real loaded world, in four parts:

  RULES        mechanics with one right answer, each citing the source that
               settles it.

               READ THIS BLOCK AS CLOSED-BOOK. These are the rules of a CUSTOM
               game: no model has read them, so a right answer is the model
               reasoning its way to the mechanic a designer would pick, and a
               wrong one is not ignorance of something it was taught. It is a
               baseline for the open-book case -- what the model scores once the
               game hands it the rule -- and the gap between the two is the
               thing worth tracking. Scored here because a model that guesses
               these well needs less told to it in a prompt, which is prompt
               budget saved every turn.
  READING      the game's OWN precomputed tool answers as context -- exactly
               what Game_Llm.cpp hands a model before it writes -- and
               questions whose answers are in that text. This is the path the
               advisor really runs, so a failure here is a failure in play.
  TOOL         which of the real tools answers a given question. The catalogue
               is PARSED from src/llm/Advisor.cpp rather than restated, so the
               test cannot drift from the tools the game offers.
  CONSEQUENCE  short causal chains this project has already measured, where
               the plausible answer and the true one differ.

EVERY SCORE IS REPORTED AGAINST ITS CHANCE LEVEL. Four options means 25% for a
model that understands nothing, and a bare percentage hides that -- memory
read-an-effect-against-its-chance-value. A category at 30% is noise, not
partial credit.

Usage:
    python3 tools/llm_comprehend.py                     # endpoint from data/config.json
    python3 tools/llm_comprehend.py --model llama3.1:8b
    python3 tools/llm_comprehend.py --no-state           # skip the world load
    python3 tools/llm_comprehend.py --json out.json      # for tracking over time
"""
import argparse, json, os, pathlib, re, subprocess, sys, urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent

# ── The question bank ───────────────────────────────────────────────────────
# Each entry: question, options, answer index, and the SOURCE that settles it.
# A question whose source cannot be named does not belong here: this file is
# the ground truth other people will argue with, so it has to be auditable.

RULES = [
 ("Two armies fight over a province. Both stacks are far larger than the "
  "province's combat frontage. What does sending even more men do to the odds?",
  ["Improves them in proportion to the extra men",
   "Improves them, but with diminishing returns",
   "Nothing — above the frontage both sides' power is capped",
   "Worsens them, because large stacks suffer supply penalties"], 2,
  "resolveAssault caps both sides at combatWidth(pid)"),

 ("The frontage a force consumes is computed how?",
  ["By headcount alone",
   "By headcount weighted by troop kind — mechanised take less than militia",
   "By the province's population",
   "By the attacker's fortification level"], 1,
  "Game_TurnLogic.cpp: 'the frontage is consumed by men WEIGHTED BY THEIR KIND'"),

 ("A country runs its 1st, 2nd and 3rd monument. What does the THIRD active "
  "slot cost per turn?",
  ["50", "75", "125", "200"], 2,
  "odmon::slotCost = 50 + 25*T(n-1): 50, 75, 125, 200, 300, 425"),

 ("Taking a monument down costs:",
  ["Nothing — you recover its build cost",
   "Half its build cost",
   "A flat 50, the same for every kind",
   "It scales with the monument's level"], 2,
  "Monuments.h kDismantleCost = 50.0f, 'a decision, not a sale'"),

 # REWRITTEN ONCE, and the first version was the defect. It offered "Nothing --
 # intend is a recording tool with no effect", whose first clause is TRUE
 # (intend really is a recording tool, records=true) and whose second is false.
 # A model picking it was not necessarily wrong about the game, it was picking
 # the half-true option, and two options being simultaneously defensible makes
 # the question measure the question writer -- the same fault that made the TOOL
 # distractors unfair. Now mutually exclusive on the one thing being tested:
 # does a recorded lean change what the government does, or not.
 ("You record 'more industry' with the intend tool. Does that change what your "
  "government subsequently does?",
  ["No — what you record is noted but never acted on",
   "Yes — it biases what the ministries are inclined to do, though they still choose",
   "Yes — it compels the ministries to build industry next turn",
   "Only when you are at peace"], 1,
  "Advisor.cpp intend + Game::applyLlmLean: a bounded, clamped thumb on the scale"),

 # ── RULES AN ADVISOR'S OWN ADVICE DEPENDS ON (journal 453) ──
 #
 # The system prompt in Advisor.cpp covers how to write and who you are, and its
 # own comment notes the only game-level instruction is the language. So the
 # advisor is CLOSED BOOK on rules in play, exactly as this block tests it --
 # which makes a failure here a failure in the game, and makes this block a
 # shortlist of what is worth prompt budget.
 ("Your country is already fighting one war. Your government is considering "
  "declaring war on somebody else as well. What does the game allow?",
  ["Any number of wars at once",
   "Two chosen wars at once",
   "One war it CHOSE at a time -- though being attacked, a guarantee or a call "
   "to arms still add more",
   "One war in total, however it started"], 2,
  "AI_MAX_CONCURRENT_WARS = 1: 'It restrains only wars the AI CHOOSES'"),

 ("A neighbour you are at war with offers a ceasefire. You are not losing. "
  "Refusing it keeps the war open. What does that cost you?",
  ["Nothing -- an open war you are winning is pure upside",
   "Only war weariness at home",
   "Your one war slot stays occupied, so you cannot declare war on anyone else",
   "Your existing alliances lapse"], 2,
  "journals 385-388: a refused ceasefire kept the only slot full and blocked all expansion"),

 ("Your government can win over a restive minority by funding concessions. "
  "What is the catch?",
  ["It costs nothing and is simply good policy",
   "It commits income every turn afterwards, while costing nothing at the "
   "moment you choose it",
   "It can only be done once per country",
   "It converts the minority into your own ethnic majority"], 1,
  "austerityReflex comment: conciliate 'commits income PERMANENTLY while costing nothing at the moment chosen'"),

 ("Which of these great works cannot be built inland?",
  ["A university", "A grand exchange", "A missile silo", "A megacity"], 1,
  "Monuments.h: GrandExchange needsPort = true -- 'what it does happens at sea'"),

 ("A province's ethnic composition is given as shares. To find how many people "
  "belong to a named minority there you should:",
  ["Sum the minority counts across the province",
   "Take that group's share of the province population — the shares partition it",
   "Multiply the province population by the number of groups",
   "Use the country-wide figure instead"], 1,
  "minorities partition a province; shares total 100%, never summed raw"),
]

CONSEQUENCE = [
 ("A country at war has an empty treasury but a large population and plenty of "
  "manpower left. Its war ministry wants more soldiers. What actually limits "
  "recruitment?",
  ["The manpower ceiling",
   "The money — an order is sized against the treasury, which is empty",
   "The number of provinces it holds",
   "Its fortification research"], 1,
  "journal 438: raising the manpower cap is a null; budgetCount binds"),

 ("An AI country spends 41% of its income on research and under 1% on its army, "
  "and keeps losing ground. Moving research money into the army would:",
  ["Clearly help, since the army is what holds ground",
   "Not obviously help — research compounds harder at this horizon, and it was "
   "measured as a loss",
   "Help only for coastal countries",
   "Have no effect either way, as both are rounding errors"], 1,
  "OD_RESEARCH_BAR 0.35 and 0.25 scored 262 and 202 against 435 for 0.45"),

 ("A small country under permanent attack from one neighbour has fallen from 17 "
  "provinces to 4 by turn 40 and holds roughly that for the next 360 turns. A "
  "new rule improves how it manages its army from turn 50 onward. Its final "
  "territory will:",
  ["Improve substantially",
   "Improve slightly",
   "Barely move — the outcome was decided before the rule can act",
   "Get worse"], 2,
  "journal 411: the hood seat is decided before turn 40, nine arms span 0.034"),

 ("A country is funding pacification heavily and goes bankrupt. Bankruptcy cuts "
  "the discretionary budgets. What happens to the minorities it had been paying "
  "to keep calm?",
  ["Nothing — pacification is already paid for",
   "Their alignment drifts down, and unrest rises",
   "They are automatically assimilated",
   "The country's own ethnic majority grows"], 1,
  "memory chn-dies-to-rebellion: bankruptcy -> minority cuts -> unrest"),
]

# TOOL questions are generated against the REAL catalogue, below.
TOOL_QUESTIONS = [
 ("You want to know whether your own country can afford a war you are "
  "considering.", "our_economy"),
 ("You want to know what great works your country has raised and which it is "
  "paying to keep running.", "our_monuments"),
 ("You want to know what standing agreement -- alliance, pact or none -- you "
  "have with one named country, and whether you share a border with them.",
  "standing_with"),
 ("You want to know where your own army is posted and what kind of troops "
  "they are.", "our_forces"),
 ("You want to record, in your own words, what your country is trying to "
  "achieve.", "set_goal"),
 ("You want to know what land your own country claims but does not hold.",
  "our_claims"),
]


def parse_tools():
    """The catalogue, PARSED not restated -- the same approach as
    check_llm_tools.py, so this test cannot test a tool list the game no
    longer offers."""
    adv = (ROOT / "src/llm/Advisor.cpp").read_text(encoding="utf-8", errors="replace")
    start = adv.find("kTools[]")
    end = adv.find("\n};", start)
    if start < 0 or end < 0:
        sys.exit("llm_comprehend: could not find the kTools catalogue")
    block = adv[start:end]
    names = re.findall(r'\{"([a-z_]+)",', block)
    if len(names) < 10:
        sys.exit(f"llm_comprehend: parsed only {len(names)} tools, the format moved")
    # Descriptions as well, for --open-book: the catalogue the game really
    # shows a model, so the test measures OUR documentation and not a
    # paraphrase of it.
    #
    # COMMENTS STRIPPED FIRST, and this was a real defect rather than tidiness.
    # The first version pulled every string literal out of the entry body --
    # INCLUDING the quoted phrases inside // comments. A comment beside `intend`
    # explaining that a model had wrongly answered "intend is a recording tool
    # with no effect" was therefore handed to the model as part of the tool's
    # own description, in open-book mode, as fact. It duly agreed, and the run
    # read as "our documentation misleads the model" when the truth was "the
    # test quoted the wrong answer at it". Guarded below as well.
    nocomments = re.sub(r'//[^\n]*', '', block)
    descs = {}
    for name, body in re.findall(r'\{"([a-z_]+)",(.*?)\}(?=,\s*(?:\{|$)|\s*$)',
                                 nocomments, re.S):
        parts = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
        if parts:
            # The description is the concatenation of the adjacent literals that
            # form it, which in this catalogue is everything before the argName.
            descs[name] = " ".join(" ".join(parts).split())
    # The world-rules block the prompt really sends, parsed from between the
    # markers in systemPrompt so the brief this test supplies cannot drift from
    # the game's wording. Comments inside the region are stripped: a quoted
    # wrong answer in a comment is not an assertion (journals 445, 452).
    rules = []
    rb, re_ = adv.find("WORLD-RULES-BEGIN"), adv.find("WORLD-RULES-END")
    if rb >= 0 and re_ > rb:
        region = re.sub(r'//[^\n]*', '', adv[rb:re_])
        for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', region):
            lit = lit.replace("\\n", " ").strip()
            if len(lit) > 20:
                rules.append(lit)
    globals()["WORLD_RULES"] = "\n".join(rules)
    bad = [n for n, d in descs.items() if "//" in d]
    if bad:
        sys.exit(f"llm_comprehend: comment text leaked into {bad}; parser is wrong")
    return names, descs


def world_state(iso, binary):
    """Ground truth for READING: the game's own tool answers for one country in
    a real loaded world, captured through the OD_LLM_TOOLS door (journal 436)."""
    # THROUGH THE GATE. LOOP.md hard rule 3: every game process goes through
    # tools/odlock.py, which caps concurrent games at two on this 16 GB machine.
    # The first draft of this file shelled straight to the binary and would have
    # been a third game beside a running bench.
    env = dict(os.environ, OD_LLM_TOOLS=iso)
    try:
        out = subprocess.run([sys.executable, "tools/odlock.py", "--",
                              str(binary), "--check"], env=env, cwd=str(ROOT),
                             capture_output=True, text=True, timeout=1800).stdout
    except Exception as e:
        print(f"  (state skipped: {e})")
        return None
    answers = {}
    for line in out.splitlines():
        m = re.search(r'\]\s{2,}([a-z_]+): (.*)$', line)
        if m and m.group(1) not in ("advisor",):
            answers[m.group(1)] = m.group(2).strip()
    return answers or None


def reading_questions(st):
    """Questions whose answers sit in the text the game itself would hand the
    model. Built from the live dump so they describe THIS world, not a
    remembered one."""
    qs = []
    terr = st.get("our_territory", "")
    if "no land at all" not in terr and terr:
        size = ("a small country" if "small country" in terr else
                "a country of middling size" if "middling" in terr else
                "a large country" if "large country" in terr else
                "one of the great powers" if "great powers" in terr else None)
        if size:
            opts = ["a small country", "a country of middling size",
                    "a large country", "one of the great powers of this world"]
            qs.append(("Given the briefing, your country is best described as:",
                       opts, opts.index(size), "our_territory"))
        qs.append(("Does your country have ports on the sea, according to the briefing?",
                   ["Yes", "No", "The briefing does not say", "Only on rivers"],
                   0 if "ports on the sea" in terr else 2, "our_territory"))
    mon = st.get("our_monuments", "")
    if mon:
        qs.append(("How many great works has your country raised, per the briefing?",
                   ["None", "Exactly one", "Several, all running",
                    "Several, some standing idle"],
                   0 if "no great works" in mon else
                   (3 if "idle" in mon else 2), "our_monuments"))
    stock = st.get("our_stockpiles", "")
    if stock:
        qs.append(("What does the briefing say about your materiel?",
                   ["Your storehouses are full",
                    "You are short of munitions specifically",
                    "Your ministries keep no separate books for materiel in this world",
                    "Your materiel is held by your allies"],
                   2 if "no separate books" in stock else 1, "our_stockpiles"))
    econ = st.get("our_economy", "")
    if econ:
        ahead = ("putting money by" in econ or "a little ahead" in econ)
        qs.append(("Per the briefing, your treasury position each turn is:",
                   ["Ahead — you are putting money by or a little ahead",
                    "Spending more than you take in",
                    "Exactly balanced",
                    "The briefing does not say"],
                   0 if ahead else 1, "our_economy"))
    res = st.get("our_research", "")
    if res:
        qs.append(("What share of the country's earnings do your institutes take, "
                   "per the briefing?",
                   ["A large share", "A fair share", "Very little or nothing",
                    "The briefing does not say"],
                   0 if "large share" in res else
                   (1 if "fair share" in res else 2), "our_research"))
    return qs


def ask(endpoint, model, key, system, prompt, timeout=120):
    body = {"model": model, "temperature": 0, "stream": False,
            "messages": [{"role": "system", "content": system},
                         {"role": "user", "content": prompt}]}
    req = urllib.request.Request(
        endpoint.rstrip("/") + "/chat/completions",
        data=json.dumps(body).encode(), method="POST",
        headers={"Content-Type": "application/json",
                 **({"Authorization": f"Bearer {key}"} if key else {})})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        d = json.loads(r.read())
    return d["choices"][0]["message"]["content"]


LETTERS = "ABCD"
SYSTEM = ("You advise the government of a country in a grand-strategy game. "
          "Answer each question with a SINGLE LETTER and nothing else.")


def run_block(name, items, endpoint, model, key, tool_names=None, context=None):
    """items: (question, options, answer_index, source). Returns (right, n, rows).

    `context` is the briefing -- the game's own precomputed tool answers. The
    first version of this function did not take one, so the READING block asked
    "per the briefing" with no briefing attached and the model answered "the
    briefing does not say" four times, which was CORRECT. It scored 2/6 and the
    bug read exactly like a model that could not read. Fixed; and the lesson is
    memory wiring-checks-are-not-execution pointed at the test itself.
    """
    right, rows = 0, []
    for q, opts, ans, src in items:
        listing = "\n".join(f"{LETTERS[i]}. {o}" for i, o in enumerate(opts))
        head = (f"Your ministries have sent you this briefing:\n\n{context}\n\n"
                if context else "")
        try:
            reply = ask(endpoint, model, key, SYSTEM, f"{head}{q}\n\n{listing}\n\nAnswer:")
        except Exception as e:
            rows.append((q, "ERR", LETTERS[ans], str(e)[:40], "", opts[ans])); continue
        m = re.search(r'\b([A-D])\b', reply.strip().upper())
        got = m.group(1) if m else "?"
        ok = (got == LETTERS[ans])
        right += ok
        rows.append((q, got, LETTERS[ans], src,
                     opts[LETTERS.index(got)] if got in LETTERS else "",
                     opts[ans]))
    return right, len(items), rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--endpoint"); ap.add_argument("--model"); ap.add_argument("--key")
    ap.add_argument("--iso", default="FRA")
    ap.add_argument("--binary", default="build/OpenDoctrinesServer")
    ap.add_argument("--no-state", action="store_true")
    ap.add_argument("--json")
    ap.add_argument("--open-book", action="store_true",
                    help="prepend the real tool catalogue, as the game shows it. "
                         "The gap between closed and open book is how much of a "
                         "comprehension failure OUR OWN wording is responsible for.")
    a = ap.parse_args()

    cfg = {}
    cfgp = ROOT / "data/config.json"
    if cfgp.exists():
        try: cfg = json.loads(cfgp.read_text())
        except Exception: pass
    endpoint = a.endpoint or cfg.get("llmEndpoint") or "http://127.0.0.1:11434/v1"
    model = a.model or cfg.get("llmModel") or "llama3.1:8b"
    key = a.key or cfg.get("llmApiKey") or ""
    print(f"endpoint {endpoint}   model {model}   "
          f"{'OPEN book' if a.open_book else 'closed book'}\n")

    briefing = None
    tools, tool_descs = parse_tools()
    tool_items = []
    for q, correct in TOOL_QUESTIONS:
        if correct not in tools:
            print(f"  (tool question skipped, '{correct}' is no longer offered)"); continue
        # SPREAD, not the first three. The first draft took tools[:3], which
        # are standing_with / who_is_fighting / strength_of -- all about other
        # countries, so a war question drew three near-synonyms and the model's
        # "wrong" answer was defensible. A distractor has to be clearly wrong or
        # the block measures the question writer, not the model
        # (memory unequal-standards-fake-a-hit-rate).
        others = [t for t in tools if t != correct]
        step = max(1, len(others) // 3)
        distract = [others[(i * step + 1) % len(others)] for i in range(3)]
        distract = list(dict.fromkeys(distract))
        while len(distract) < 3:
            for t in others:
                if t not in distract and t != correct:
                    distract.append(t)
                    if len(distract) == 3: break
        opts = sorted([correct] + distract[:3])
        tool_items.append((f"{q} Which tool do you call?", opts, opts.index(correct),
                           "parsed from Advisor.cpp"))

    blocks = [("RULES", RULES), ("CONSEQUENCE", CONSEQUENCE), ("TOOL", tool_items)]

    if not a.no_state:
        binary = ROOT / a.binary
        if binary.exists():
            print(f"loading a world for READING questions ({a.iso})...")
            st = world_state(a.iso, binary)
            if st:
                rq = reading_questions(st)
                # The briefing the model is shown: the same prose Game_Llm.cpp
                # precomputes for a letter, in the same order.
                briefing = "\n".join(f"- {k}: {v}" for k, v in st.items()
                                     if k.startswith("our_") or k == "incoming_requests")
                print(f"  {len(st)} tool answers captured, {len(rq)} questions built\n")
                blocks.append(("READING", rq))
            else:
                print("  no state captured; READING skipped\n")
        else:
            print(f"  {binary} missing; READING skipped\n")

    total_r = total_n = 0
    out = {"endpoint": endpoint, "model": model, "blocks": {}}
    print(f"{'category':<13}{'score':>9}{'chance':>9}   verdict")
    print("-" * 56)
    for name, items in blocks:
        if not items: continue
        ctx = briefing if name == "READING" else None
        if name in ("RULES", "CONSEQUENCE") and globals().get("WORLD_RULES"):
            # What systemPrompt actually sends, so RULES is scored against the
            # briefing the advisor really gets rather than against nothing.
            ctx = ("How this world works:\n" + globals()["WORLD_RULES"]
                   + (f"\n\n{ctx}" if ctx else ""))
        if a.open_book and name in ("RULES", "CONSEQUENCE", "TOOL"):
            book = "\n".join(f"- {t}: {tool_descs.get(t,'')}" for t in tools)
            ctx = (f"These are the tools your government offers you, with what "
                   f"each one does:\n{book}" + (f"\n\n{ctx}" if ctx else ""))
        r, n, rows = run_block(name, items, endpoint, model, key, tools, context=ctx)
        chance = 25.0
        pct = 100.0 * r / n
        verdict = ("above chance" if pct > chance + 100.0 / n else
                   "at chance — no comprehension shown")
        print(f"{name:<13}{r:>4}/{n:<4}{chance:>8.0f}%   {pct:5.1f}%  {verdict}")
        out["blocks"][name] = {"right": r, "n": n, "pct": pct,
                               "rows": [{"q": q[:80], "got": g, "want": w, "src": s,
                                          "chose": ch, "wanted": wd}
                                        for q, g, w, s, ch, wd in rows]}
        total_r += r; total_n += n
    print("-" * 56)
    if total_n:
        print(f"{'TOTAL':<13}{total_r:>4}/{total_n:<4}{25:>8}%   "
              f"{100.0*total_r/total_n:5.1f}%")
        out["total"] = {"right": total_r, "n": total_n, "pct": 100.0*total_r/total_n}
    print("\nWrong answers, with the source that settles each:")
    for name, b in out["blocks"].items():
        for row in b["rows"]:
            if row["got"] != row["want"]:
                print(f"  [{name}] {row['q']}")
                chose = row.get("chose")
                print(f"      said {row['got']}{f' ({chose})' if chose else ''}, "
                      f"answer {row['want']} ({row.get('wanted','')}) -- {row['src']}")
    if a.json:
        pathlib.Path(a.json).write_text(json.dumps(out, indent=2))
        print(f"\nwrote {a.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
