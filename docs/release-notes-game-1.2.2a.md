Mostly reported bugs, and three of them stopped people playing.

- **Windows could not sign in.** The game looks for the system's trusted
  certificates in seven places and every one of them is a UNIX path. Windows
  does not keep its roots in a file at all — they live in the Schannel store,
  reachable only through CryptoAPI — so on Windows the search found nothing and
  every HTTPS call was refused with "no system certificate store was found".
  Sign-in, the account service, update checks, the announcement board and
  feedback: all of them, on Windows only. macOS finds `/etc/ssl/cert.pem` and
  the web build never runs that code because the browser does its own TLS,
  which is exactly why it was reported as "works on Mac and web".

  Refusing was right. Connecting anyway with verification off would have meant
  accepting any certificate on the one platform least able to notice. The bug
  was that Windows had no way to succeed.

- **Swiping a list on a phone now scrolls it.** There was no one-finger scroll
  gesture at all. Every one of the sixteen scrollable panels reads the mouse
  wheel, and the wheel was set in exactly one place: the two-finger pinch. So
  the shipped answer to "scroll this list" on a phone was "pinch it", which is
  also the zoom gesture, and a swipe did what the touch layer said it did —
  moved the cursor and held the left button down.

- **Doctrines a player never chose were being enacted.** Reported as "sometimes
  policies start being implemented when i dont even implement them": enact Free
  Press, and Open Borders appears. Every row in the doctrine list hit-tests its
  own Enact button, and the list's clipping rectangle clips the drawing and not
  the clicks — so a doctrine scrolled out of view kept a live, invisible button
  somewhere else on the screen. With the list scrolled it sits under the Close
  button. The same fault is fixed in the claims list, where it would have added
  or dropped a claim.

- **Advanced Shipbuilding does what it says, and so do sixteen other effects.**
  Reported by a player: "I researched Advanced Shipbuilding, which says it
  reduces ship cost by 10%. Yet the cost to build ships was not reduced." It was
  not. `navyCostPct` was summed correctly by the research resolver and then read
  by nothing. Four research nodes and five doctrines advertised it. Three more
  levers were in the same state — army upkeep, passive income and the resource
  and population income modifiers. All seventeen effects the resolver can sum
  are now spent somewhere, and `tools/check_effect_fields.py` fails the build if
  that stops being true.

- **Four screens asked about a province against the wrong country's compass.**
  Unrest is a province's distance from ITS government, and the short form of
  that call answers for whoever the player is. So a mod asking about a province
  it did not own, the advisor summarising a seat's own districts, and a map
  script reading `rebellion_chance` were all given a number about somebody else.

- **A world now records what it was last loaded with** — the game version and
  the mods — so loading a campaign without a mod it was built with says so
  instead of quietly dropping what that mod added.

- **The mod directory is in the game**, the looking-for-a-game board is on the
  main menu, and listings post as forum threads rather than messages.

- `--no-tunnel` no longer publishes a public address anyway. C++ mods build
  again. The dev view publishes at the game's own resolution instead of a fixed
  480x270.
