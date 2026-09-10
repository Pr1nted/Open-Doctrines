# Changelog

## Unreleased

- **The map as a globe.** Press **F7** and the world wraps onto a sphere you can
  turn: drag to spin it, wheel to zoom, click a province exactly as before. The
  ground you were looking at stays in front of you across the switch, both ways.

  It costs less than it sounds because of two things the map already was. The
  political layer is an **equirectangular raster**, which is the projection a
  sphere wants — so the globe needs no new art and no second copy of the map.
  And a province is identified by its **pixel colour**, so picking is ray→sphere,
  sphere→latitude and longitude, then the same lookup the flat map already does.
  There is no second pick path to keep in step with the first.

  Every overlay comes across for the same reason: both views composite the same
  stack of full-map textures, so claims, population, resources, districts,
  borders and the editor's own layers all arrive without a line of globe-specific
  code. The one thing the flat projection never had to say is *this point is
  behind the planet* — that is answered once, where map positions become screen
  positions, rather than at each of the places that draw a marker.

- **Day and night, and the season.** A terminator you can tune: how dark the
  unlit half goes, how bright the lit half is, how wide dawn is, and what colour
  the sun is. `night_floor` defaults well above black on purpose — a fully dark
  night side makes half an empire invisible, and the map is a working document
  before it is a picture.

  The month moves the sun's **declination**, not its longitude: longitude is time
  of day, declination is the season. In June the northern hemisphere leans into
  the light; in December the reverse.

- **Weather, in the air rather than on the ground.** Cloud sits inside the
  atmosphere at a settable height and casts shadows on the ground beneath it —
  which is what puts it above the map instead of on it. It thickens as you zoom
  in, because from orbit you want the ground through the pattern and close in you
  are looking along a much longer path of air.

  The field is built from **cellular noise** organised by cyclonic rotation.
  Summed smooth noise gives torn wool however many octaves you add: real weather
  turns around lows, and a cumulus field is discrete cells with clear air between
  them. Both are generated, not shipped, and both are baked on a worker thread —
  on a browser, where there are no threads to bake on, at a quarter the size.

- **A sky each map carries.** Stars, a moon that takes the same light as the
  planet and therefore shows a phase, and a sun you can turn towards. Eclipses
  fall out of the same term asked from two places: the moon shadowing the ground
  is a solar eclipse, the planet shadowing the moon is a lunar one.

  All of it is authored per map in the editor's Metadata panel and stored as
  `sky.json` inside the `.odmap`. Every field is optional, so a map written
  before any of this existed loads with Earth-like defaults rather than being
  refused — which matters in a format that already carries 118 files.

- **The world unrolls into the globe instead of cutting to it.** F7 no longer
  swaps one picture for another: the map lifts off the flat, curls, and closes
  into a sphere over about seven tenths of a second, with the camera swinging
  round to the longitude you end up looking at. The same vertex knows both of
  its homes — where it sits on the sphere, and where it sits on a flat sheet cut
  from the same texture coordinates — so the whole move is one blend between
  them and costs nothing but the blend. Daylight comes back as it flattens,
  because a flat map has no night side, and the cloud and atmosphere shells hold
  off until the planet is round enough to wear them. Counters, labels and clicks
  sit the animation out: mid-unroll a place is at neither of the two positions
  those paths know how to compute, and half a second of nothing reads as part of
  the move where half a second of markers in the wrong place reads as a fault.

- **A button for the globe, not just a function key.** Under Settings in the
  right-hand column, labelled with where it will take you rather than where you
  are. F7 still works; a function key is a thing you have to be told about.

- **The unroll starts from where you were looking.** The animation used to open
  on a whole-world shot no matter how far in you were zoomed, so its first frame
  was itself a jump. The sheet is now framed on a camera that reproduces the 2D
  view exactly -- same centre, same zoom -- and the same is done in reverse, with
  the flat camera settled on the globe's position and zoom before the sphere
  starts to unroll. Asserted rather than eyeballed: the map pixels the 2D view
  puts on each screen edge land on those same edges here.

- **Ship routes stay on the planet.** Route tracks and army arrows were still
  projected with the flat map's tile-wrapping, which on a globe drew lines
  straight off the edge of the screen. They go through the projection now, a leg
  with an end round the back is dropped rather than drawn to a sentinel, and long
  legs are subdivided so a track from the Channel to the Cape follows the ocean
  instead of chording through the planet. The flat map draws exactly the line it
  drew before.

- **Country names follow the surface of the globe.** Each letter is placed
  through the same projection as everything else, so a name bends with the ground
  it sits on and a country past the horizon takes its name with it. Two things
  the flat map never needed: names shrink and fade toward the limb, where a
  continent's worth of ground is seen edge-on and occupies a few pixels; and
  where names would overprint — thirty countries in a hand's breadth of Europe,
  with no zooming in to escape it — the larger country keeps its name and the
  smaller ones yield. The flat map's labelling is untouched.

  Every name is written ON the globe -- none of them turn to face the camera.
  Where a country is too small on screen to hold its name at a readable size,
  the run is widened along the surface until the letters fit, which is ordinary
  cartography: the name reaches past its own borders rather than being crushed
  inside them. The width is found by iteration, because the map-to-screen
  relation along an arc is not linear and one division undershoots it. Where
  even that cannot work -- ground seen almost edge-on near the limb, which would
  need a name wrapped a fifth of the way round the world -- the country simply
  goes unnamed, as it would in an atlas.

  Names are also decluttered against the run they actually occupy, sampled
  through the projection, rather than against a box guessed from their letter
  count: a country's name is set along its own axis and usually bowed, so the
  box claimed room the name did not use and missed room it did.

- **The Admin screen says how many accounts exist.** Counted by walking the keys
  rather than kept as a running total: the store has no atomic increment, so two
  sign-ups landing together would both read the same number and write the same
  number back, losing one permanently with nothing able to notice afterwards. A
  count that drifts quietly downward is worse than no count, because it still
  looks like a fact.

## game 1.2.0a

- **OpenDoctrines plays inside Discord.** The game is an Activity now: launch it
  from the rocket button in a voice channel and it runs in the client, with no
  download and no install. Whatever is deployed is what everybody in that channel
  is playing, the moment it goes up.

  The work was mostly in one place, and it is not the obvious one. Inside an
  Activity a Content Security Policy allows exactly the hosts named in the app's
  URL mappings, so every request the game makes has to be rewritten to go through
  Discord's proxy. What is deliberately **not** rewritten is the account service's
  identity: a host compares the issuer named in a join ticket against its own, so
  repointing it would have made an Activity player disagree with every host in
  existence. The transport is diverted; the identity is not.

- **You can join a game with the invite code alone.** Leave the address blank and
  the game goes through the account service's relay: no port to forward, no tunnel
  to keep alive, and the host never learns your IP address — so the panel warning
  you about that is shown only when you actually type an address.

  This is the only way a player in a browser or in Discord can join at all, since
  a sandboxed Activity cannot open a socket to somebody's home connection whatever
  they paste in. Hosts opt in with **Host through the account service** on the host
  screen, or `relay: true` in a dedicated server's config.

  The trade is stated plainly because it is real: a relayed game needs the account
  service for the whole of its life, where a listening host only needs it to start.

- **A dedicated server could never open a session, and now can.** It skipped
  `Game::init()` — correctly, because that opens an OpenGL window — and that is
  also where the game's config is read and the account client is set up. So it ran
  with no issuer, no token and no credential, and failed at the last step with
  "Sign in and register this server before hosting": advice that could not be
  followed, because the machine *was* signed in and this process had simply never
  looked.

  Two more things were hiding behind that. The failure check ran once,
  synchronously, before the asynchronous open could possibly have failed — so a
  later refusal was never looked at again and the server sat silent for ever with
  no join code and no error. And a server credential that had stopped being valid
  was unrecoverable, because registration only ran when the field was empty. All
  three are fixed; a failed open now says why, clears a dead credential, and exits
  non-zero instead of telling a supervisor it shut down cleanly.

- **Telling us how long you played, if you want to.** A new Advanced setting,
  **off by default and off after every update**. When it is on, the game sends one
  message at the end of a session: how long it lasted as one of five ranges, and
  whether you were on web, desktop or Android. That is the entire message — no
  account, no nickname, no installation id, no device, no address.

  Because nothing links two reports, **there is no "delete mine" and we do not
  pretend to offer one**; every report expires by itself after ninety days, and the
  whole set can be erased on request. It also means returning players cannot be
  counted, which is a real thing given up on purpose: counting them needs an
  identifier, and an identifier is the thing being refused. PRIVACY.md and TERMS.md
  now say all of this, including the sentence that used to promise no usage
  reporting at all.

- **Paste works in text fields.** Ctrl+V and Cmd+V were handled, but the web build
  called a desktop clipboard function that returns nothing in a browser — so in
  Discord every field silently ignored a paste, including the one you put an invite
  code into. The page now listens for the browser's own paste event, which needs no
  permission because the keypress is the consent.

  Four screens had never called the shared editing code at all, so paste had never
  worked in them on any platform: the admin fields, the bug report, the moderator's
  note and the account lookup. They keep their own typing rules — the duration
  field still refuses anything but a duration — and take the paste through one
  shared path.

- **The caret sits at the end of what you typed.** It was worked out after the
  wrapping loop, from a line buffer the loop's last pass had just cleared and a y
  position it had just advanced — so it sat at the left margin, one line below the
  text. Fixed once in the mail composer months ago; the fix did not travel, because
  a copied loop does not inherit a later one. Three more copies had it. All four
  fields now draw through one function, and the screenshot that photographs that
  screen focuses a field so the caret is actually in the picture.

- **The Android build reports its own version.** `AndroidManifest.xml` carried
  `versionCode="1"` and `versionName="1.0.6a"` as literals while every other
  platform stamped itself from the `VERSION` file — so the APK announced 1.0.6a
  and every build ever made declared version code 1. That is the value **every app
  store refuses an update on if it does not increase**, so the next upload to any
  of them would have been rejected. Both are stamped at packaging time now, and the
  build fails if a placeholder survives.

- **The hosted web build ships the game, not just the menu.** The deploy script
  copied four files and left `data/` behind — 61 MB of maps, music, fonts and the
  AI model that are served next to the page rather than inside the preload. Every
  deploy since hosting was set up put a playable menu online with nothing behind
  it. It was invisible because the host answers a missing path with `index.html`
  and status 200, so the game asked for a 1.2 MB map, was handed 10 KB of HTML, and
  reported that the download had failed. The deploy now stages `data/`, ships a
  404 page so a missing file fails as one, and checks the live URL afterwards
  rather than trusting the upload.

- **Mail: letters between countries, delivered when the turn resolves.** A new
  Mail button opens a correspondence with any country you may write to — one
  thread each, read like a messenger. Write now, it arrives next turn, and until
  the turn resolves it is still on your desk: **you can rewrite it or tear it
  up**. Once delivered it is history and cannot be touched.

  That delay is the whole design rather than a limitation. It is the one thing a
  chat box cannot offer — the chance to change your mind — and it is what makes
  an AI correspondent fair, because a bot that answered instantly would out-talk
  every human at the table.

  The button appears **only when somebody could answer**: on a server, or with
  the language-model module loaded. In a single-player game with neither, it is
  absent rather than greyed out — a greyed control invites you to work out how to
  enable it, and there is nothing to enable.

  Hosts choose who may write: nobody, players only, advisors only, or everyone.
  Players choose who may write **to them**, and that always wins — a host can
  narrow who speaks on their server, but nothing a host permits forces a letter
  on somebody who has shut their door. Hosts can also keep a list of words their
  server will not carry, which is a blunt instrument and is documented as one.

  Map scripts can read `game.llm_diplomacy` and `game.mail_enabled`, so a
  scenario written around bargaining with a machine can tell.

- **AI advisors: countries that answer their own mail.** An optional module
  points the game at a language-model runner — one it installs for you, one you
  already run, or a remote API with your key. Countries then write back, in your
  own language, and they talk to each other as well as to you.

  **They are allowed to lie.** A diplomat who cannot is not a diplomat, so they
  bluff, flatter and promise things their country will not do. What they may
  never do is pretend to be human: every letter carries a **bot** tag, in every
  language, on every screen that shows it. The lying is a game mechanic;
  concealing the speaker would be a lie about the game itself.

  **Each correspondence is sealed.** The model writing as Britain to France sees
  the Britain–France thread and nothing else — not Britain's other letters, not
  the world state, not your orders. It is given a coarse word for relative
  strength rather than a number, because a correspondent who can quote your army
  size is reading the save file and every player can tell.

  It writes letters and nothing else. The existing AI still runs the war, the
  budget and the diplomacy — that AI is measured and deterministic, and handing a
  language model the levers would trade all of that away for something nobody
  can bench.

  Installing a runner downloads and executes third-party software, so: the
  version is **pinned in the source**, the archive is checked against a
  **SHA-256 compiled into the game** before anything is unpacked, it comes over
  HTTPS from the project's own release host, you see the URL and size before a
  byte moves, and it lives under the game's data directory so uninstalling is
  deleting one folder. **An unpinned build refuses to install at all** rather
  than running something nobody verified.

- **Reporting a player, and moderation.** A message can be reported two ways,
  and they do different things: to the **server owner**, who can remove that
  person from their game, or to the **account service**, which can ban or time
  out the account everywhere. An account carrying the developer badge gets a
  review queue in-game, with the message, whatever context the reporter
  attached, and three outcomes — ban, timeout, or dismiss.

  **Dismissing is a real outcome.** A queue whose only clearing action is a
  punishment is a queue that punishes people.

  Bans and timeouts are announced to a Discord channel, saying **who and until
  when and nothing else** — never the reported message, never the reporter. A
  moderation channel is still a room full of people, and republishing abuse in
  order to announce that it was dealt with hands it a second audience. A
  dismissal is never announced at all.

  Reports are readable only with the developer badge and are **deleted after 90
  days**. The terms now say plainly that player conduct is not ours, and the
  privacy policy says exactly what a report stores and why it must.

- **A public tournaments noticeboard**, readable without signing in — a
  noticeboard you must log in to read is not one.

- **An optional, local, self-declared age prompt**, off by default and toggled
  by the host. It is stored on the player's machine and transmitted nowhere.
  It is **not** described anywhere as satisfying the Online Safety Act, because
  self-declaration is not "highly effective age assurance" and saying otherwise
  would be worse than not having it.

- **You can report a bug from inside the game.** Escape → Report a problem, or
  the Report button in the map editor's toolbar. Pick what it is about — the
  interface, the AI, maps and data, multiplayer, map scripting, mods and the
  SDK, security, or something else — write a couple of sentences, and send. The
  same form sends a suggestion: it is the first choice on it.

  The reason it is in the game rather than on a forum is that a bug reported
  where it happened arrives with the version, the platform and the state the
  game was in, and the same bug reported an hour later arrives as "it crashed
  sometimes". That gap is most of the work of fixing it, and it is currently
  paid by the person least able to pay it.

  Ticking **"attach what the game knows about itself"** adds the version, the AI
  version, the window size and language, the turn, the map's size, which country
  you are playing, your difficulty, every installed mod and any errors the map's
  scripts reported. **"See exactly what"** shows that text in full, scrollable,
  before anything is sent — there is no summary, because the promise is that you
  can read what leaves your machine and a promise about a summary is not that
  promise. Save and mod paths are shown as `~/...`: on all three desktop
  platforms the home directory contains your account name, and a report that
  mentions a file would otherwise mention a person.

  **A security report is never filed as a public issue.** It becomes a private
  draft security advisory on the repository — readable only by people with admin
  access, and the same object a coordinated disclosure is published from. Not a
  public issue, and not the ordinary bugs channel either: an unfixed exploit
  should not be readable by everyone who can see a chat channel. If it cannot be
  filed there it is refused outright and you are told so, rather than being
  delivered somewhere less private to make it succeed. The form says all of this
  when you pick the category.

  Spam is bounded in two places, because a client-side limit is a courtesy
  rather than a defence: the game refuses a second report within 45 seconds and
  an eleventh in a day, telling you *before* you type it rather than after; and
  the relay applies its own per-address limit, a per-install daily cap, a
  six-hour window that swallows the same report sent twice, and size caps
  enforced before the body is parsed. Nothing is sent unless you press Send —
  there is no background telemetry anywhere in the game. See `net/PRIVACY.md`.

  Reporting is optional infrastructure: with no destinations configured the
  endpoint still validates and rate-limits and simply forwards nowhere, which is
  the right behaviour for a fork that has not set up a tracker of its own.

- **Closing a GitHub issue tells the Discord thread it came from.** A report
  opens a forum post and an issue; closing or reopening the issue now posts
  "Closed by <name>", with a link, into that thread. The pain it removes is
  people still arguing in a thread about something fixed a week ago.

  One direction, deliberately. GitHub can push events to a URL and Discord
  cannot, so the reverse would need a bot holding a gateway connection — a
  permanently-connected Durable Object and a second always-on credential, which
  is a different project. And the note is posted rather than the thread being
  archived, because a webhook can only send: locking a thread needs a bot token
  too, for a tidier result rather than a different one.

  Setting it up is a step in `add-feedback.sh`, which generates the shared
  secret and creates the webhook.

- **Reporting needs an account, and reports are signed.** Every bug and
  suggestion is published — to the community channel, and a bug to the public
  issue tracker — so each carries the reporter's nickname, and the form says so
  before you type. **"Do not show my name"** publishes "Anonymous (signed in)"
  instead: the service still knows who you are, so the limits and the ban list
  are untouched, you have just asked not to be named in public. The byline comes
  from the sign-in token and never from anything the game sends, so nobody can
  file a report under someone else's name.

  The obvious cost is that the reports worth most — from players who cannot get
  past the menu — are the ones least able to sign in. That is a real trade and
  it was made deliberately.

- **The game asks once whether you are enjoying it.** After 45 minutes, between
  turns, in the corner: **Rate on itch.io**, **Something's wrong**, or **Not
  now**, which means never. It is not shown again whether or not it is answered,
  because the second time of asking gets a worse answer than the first.

  It sends you to itch.io rather than collecting five stars in the game, because
  a number only the maintainer sees does nothing for anybody, while a rating on
  the page is what somebody deciding whether to try the game actually reads.
  "Something's wrong" opens the report form — a player who is unhappy is about
  to say so somewhere, and a bug report is a better destination than a one-star
  review, for them and for the game.

  It also closed a hole. A rating needed no account and still posted to the chat
  channel, rationed only by an install ID the client invents — an open pipe into
  a Discord server. There is now no unauthenticated write path at all.

- **The pause menu is translated.** It measured and drew its four items as raw
  English while every other menu in the game goes through `T()`. Nobody noticed
  because the words are short.

- **Loading a save no longer doubles every doctrine.** The load sequence is
  unload → map load → save load, and the map load runs `applyStartingPolicies`,
  so both the active-policy list and its per-country index were already full
  when the save was read — and the save's policies were APPENDED to them. Every
  continued game therefore held two of each doctrine: **doubled doctrine upkeep,
  and every "reduces unrest" doctrine counted twice**, for the player and for
  every AI country. The only clear lived in `unloadGameData`, which runs before
  the map load rather than between it and the save.

  It was invisible until the country profile started listing doctrines BY NAME:
  a cost that is summed silently stays hidden until something enumerates it.
  The rest of the load was audited for the same shape — a container the map load
  fills and the save then appends to — and the policy pair is the only one; the
  others assign or insert into sets, which are idempotent.

  **And it compounded.** Each load added the map's starting set to whatever the
  save already held, so a game loaded and saved repeatedly accumulated a copy
  per cycle. Measured across the 1,301 saves in `data/saves`: 48 carry
  duplicates, the worst at ×7 — 572 policy entries where 529 are distinct, a
  country paying seven times its doctrine upkeep. Stopping the doubling could
  not have repaired those, because a save carrying the key is exactly what tells
  the loader it holds the whole truth, so **duplicates are now collapsed as the
  save is read** and such a game is repaired the first time it is opened.

  The key is the whole tuple — country, policy, target province, target
  minority — not the policy id. The same doctrine can legitimately be active
  twice against different targets (an ethnic policy applied to two minorities),
  and collapsing by id would silently repeal one of them.

- **Districts can split the budget by size**, beside the existing equal split.
  It is the neutral setting rather than a convenience: the resolver divides
  budget share by ground share, so making them equal gives every district a
  factor of exactly 1.0 — the same suppression everywhere, which is what the
  country had before it was divided. Equal shares are a different thing and
  often the wrong one: on a country cut into a 23-province heartland and a
  3-province border strip, "equally" hands the strip eight times the suppression
  per province.

- **A published country lists WHICH doctrines are in force**, not just how many.
  "3 in force" is a fact about a list rather than the list, and what a reader is
  actually being told is what the government has enacted.

- **The country profile scrolls properly.** It was clamped at the top only, so
  the wheel could push the whole page off into empty space; and it now runs past
  one screen. The district list is capped at six rows with "+N more" — eleven
  districts is an ordinary number for a large country and the full list crowded
  out everything below it. The Districts tab remains where all of them live.

- **The country profile shows the division on a map**, framed on the country
  rather than on the world. A list of names and percentages says how a country
  is cut up without saying WHERE, and the same three rows describe a country
  split east-to-west and one split between a heartland and an island chain.

- **Opening Politics → Districts no longer stalls.** It cost 126 ms: 68 to build
  a province-to-pixels index over all 33 million map pixels — 128 MB, held for
  the rest of the session — and 58 more to clear, paint and upload a full-map
  RGBA overlay, which it paid again on every stroke of the district brush.

  None of that index was needed: the country's own pixels are already indexed at
  load, and the only pixels this overlay touches are that country's. Painting
  from the country instead, at half resolution (it is drawn into a panel a few
  hundred pixels wide, scaled down twentyfold before the zoom), and uploading
  only the rectangle the country occupies: **126 ms → 23 ms**, measured on the
  worst case in the game — a globe-spanning empire with 1.9 million pixels of
  ground.

- **Districts are named the way countries are, and the name follows your
  language.** They were built as "<People> <Word>" with the word translated at
  CREATION time — "Українці Територія V", a plural noun jammed against a noun,
  frozen into whichever language happened to be loaded, and unchanged when you
  switched. Two faults in one string; it read wrong in English too
  ("Ukrainians Territory").

  A district is a region, and the game already knows how to name places: the
  canonical English form is stored and rendered per language at draw time
  through the same patterns a country name uses. "Eastern Ukraine" is "Східний
  Україна" in Ukrainian and "Ost-Ukraine" in German, and it changes the moment
  you change language. A name you type yourself is yours and is never
  re-rendered.

  The compass also replaced the Roman numerals: five directions plus the bare
  place is six names before anything has to repeat, and a district that would
  have collided now takes a direction instead of a number.

  Two things it learned the hard way: a name the string table KNOWS beats a
  prettier one it does not ("Britain" transliterates to "Брітаін" — letters,
  not a word — where "British Empire" is a real name in every language), and
  the numeral has to come off before translation or " II" comes back as "ii".

- **New scripts start at the latest engine version, and there is a version 3.**
  A new script was created pinned to `#OD/MapEngine/1`, so every statement added
  since — print, for, try, and everything from this release — was refused in a
  file the editor had just made, and the author's first experience of the
  language was a lint error on a line the documentation told them to write.
  Version 3 covers `wait N turns`, `set x to`, `{value}` interpolation and
  `foreach district`; version 1 and 2 files still get exactly their own
  language, which is what pinning is for.

- **The map editor uses your accent colour.** It was a gold constant, so the one
  screen in the game that ignored the accent setting was the one people spend
  hours in. It follows the setting live, without reopening anything.

- **Districts in the map editor are not a thing you switch on.** They hid behind
  "Districts: OFF (0)" beside the paint-mode toggles, which said a country
  either has districts or does not. Every country has them — the game builds one
  covering everything the moment anybody asks — and the only question is how
  many. The list, the count and the controls are always there; the toggle that
  remains is about which of the three things a drag on the map paints.

- **The country profile has a Close button**, next to the ESC hint that used to
  be the only way out. It is a screen you reach by clicking, from a panel you
  close by clicking.

- **A model file the AI cannot read no longer looks like no model at all.**
  Every refusal in the loader — wrong magic, corrupt container, a format byte
  from another architecture, a pre-trunk file — reached a caller that printed
  "Fresh model (no file at X)" for a file that plainly existed. So pointing an
  evaluation at a model the loader dislikes did not fail: it played an
  **untrained network** and printed a perfectly ordinary score, and any number
  taken that way was a measurement of random weights wearing a trained model's
  name. A refusal now says which reason on stderr, and says that the run is
  about to play on random weights.

  The same fault in the other direction: `OD_EVAL_MODEL` pointing at a path that
  does not exist used to fall through silently to `data/ai/model.bin` and run a
  complete evaluation of a different model. It now stops.

- **The AI now plays by the player's information rules, and there was exactly
  one place it did not.** All eight of the features describing a neighbour were
  audited against what the game will actually show a player: garrisons appear in
  a province panel, industry in the industry view, borders on the map, wars and
  alliances on a country, claims on their own screen, and a treasury only when
  published. War weariness appeared **nowhere** — `warWearinessOf` had a single
  caller, the resolver — so no amount of clicking could tell you how close
  another country was to breaking, and the AI read it exactly.

  It now reads the world's mean instead of the true figure, which is what
  "everybody knows the war drags on, nobody knows who is closest to breaking"
  looks like as a number. `OD_AI_FOG_WEARINESS_OFF` restores it.

  What it costs depends on which model is playing, which is worth stating
  because the last feature measured this way did not: on the shipped model it is
  free (122 → 123 rating), and on the stronger N24 it costs 8 (187 → 179). Both
  leave survival and the worst seat untouched to the digit, so the price is
  entirely in the column that changes sign between world sets — and the reason
  for the change is symmetry with the player rather than a number.

  Everything else the AI reads about a rival is either visible per province or
  derived from what is — the difference is effort, not access. Its own trend
  features read only its own country.

- **You can see what your wars are costing you at home.** War weariness is a
  term in every province's rebellion chance and had no display in the game at
  all, so a country could slide toward revolt because of a war it answered ten
  turns ago with nothing on screen naming the cause. It now sits under the
  unrest bar in Politics, when there is any.

- **The AI module can say whose AI it is.** `Neural` stays observe-only, but a
  mod reading the feature vector had no way to ask which AI produced it — and
  that vector's layout is only stable within one ARCH. `ai_version` returns
  "ParrotZero 8.4.0", `ai_arch` returns the number that gates it, and
  `country_stance` reports what the AI has decided it is doing with a country
  (expand, consolidate, defend, develop) — the one piece of its reasoning that
  is a summary rather than a number.

- **Gearbox 1.2.** The mod ABI learns the game the game has become: districts
  (their names, budget shares, ground and regional law, readable and writable
  through the game's own rebalance so the shares still sum to 100), what a
  country publishes about itself, the army broken down by kind, a country's
  expenses, what it is worth and how many people live in it, and the research
  lock. 28 new imports; nothing renamed, re-signed or re-gated, and
  `sdk/compat/abi-1.1.json` now freezes the 1.1 surface beside the 1.0 one so a
  build that broke either fails the suite. Every 1.0 and 1.1 mod links and runs
  unchanged.

- **A country's districts are public; how they are governed is not.** The
  profile shows the division and each district's share of the budget to anybody
  who opens it — borders are visible, and so is what a government has drawn on
  them. The regional law each district runs is a fourth publishable field,
  beside expenses, doctrines and the treasury.

- **"Where the money goes" is a pie.** Four numbers to divide in your head
  became the same wedges the economy screen has always drawn — from the same
  table, so the two charts cannot disagree about what a country spends.

- **AI countries govern their districts.** They drew them and then did nothing
  with them: the budget split was the whole of it, and since their pacification
  budget is zero the split moved nothing. They now pass regional law where a
  district is in trouble, priced as a share of gross income rather than out of
  what is spare — an AI treasury is near zero as a matter of course, so a rule
  gated on cash in hand is a prohibition dressed up as a budget.

  The first version of that pricing counted the bill and ignored the income:
  a tax holiday costs 0.00 per province in cash and a tenth of the district's
  earnings, so it looked free and won 45 times out of 45. Counting income
  forgone gives a spread — tax holidays where there is little to lose, language
  rights and public works where there is.

  Measured four ways — two seed sets at two horizons, one binary with
  `OD_AI_DLAW_OFF` separating the arms:

  | instrument | rating | survival | worst seat |
  |---|---|---|---|
  | 120 turns, fixed seeds | +16 | +8 | −3 |
  | 120 turns, hold-out | +4 | −1 | −13 |
  | 400 turns, hold-out | +29 | +12 | −10 |
  | 400 turns, fixed seeds | +15 | +11 | −12 |

  Rating is up in all four and survival in three, and the floor is down in all
  four — which looked like "lifts the median seat and costs the worst one" until
  the per-seat tables were read instead of the summary line. **The floor is the
  same seat in all eight arms**, Norway in a world that rushes it, and what the
  rule does to it is this: 0.3 → 0.3, 0.4 → 0.2, 0.4 → 0.3, 0.5 → 0.3 percent of
  the world's land. A seat holding half a percent has a tiny par, so a tenth of
  a percent of land becomes fifteen points of score.

  Meanwhile the other five seats improve in most arms, by up to 3.9 points of
  land. So the rule helps five seats materially and takes a fifth of a percent
  from one that is already collapsing. Left on; `OD_AI_DLAW_OFF` turns it off in
  one variable.

- **Your chat can play a country.** Hand a country to your stream and let
  **Twitch, YouTube or Kick** decide its turn. Chat types commands, the game
  tallies them for a window, and when the window closes the winner is applied
  with everyone able to see why.

  One vote each, because a tally where the loudest typist wins is a keyboard
  test rather than a vote — a viewer may change their mind and the last thing
  they typed counts, but they cannot stack. A command is a choice from a list
  the game defines, never free text: chat is untrusted input and is treated
  that way. The reader connects anonymously, holds no credential and cannot
  post.

  The overlay feed writes **files** for OBS instead of opening a port. The
  obvious build is an HTTP endpoint on localhost, and it is the wrong one here:
  it triggers a firewall prompt on first run, during a stream, on camera.

- **Discord rich presence.** What you are playing appears under your name: the
  screen, the scenario, the country. Never a save path, an invite code, an
  account id or a session code.

  That is not a privacy setting, it is the shape of the data — the only inputs
  the code takes are a screen, a scenario name and a country name, so there is
  nothing else it *could* leak. Presence is read by strangers in every server
  you are in, which is the reason to build it that way rather than to filter it
  afterwards.

- **An announcement board on the main menu.** News, releases and tournaments,
  fetched from the account service.

  It is a **sealed format**, and that is the whole point: content from a server
  drawn by a client is the exact shape of a remote code execution bug. So a body
  is text marked up with the same parser the tutorial dialogue uses, which
  produces styled glyph runs and can do nothing else. A button cannot name an
  action — it picks one from a closed list defined in the game, and the document
  supplies at most one short parameter whose meaning the game decides. Nothing
  fetched can ask the game to open a URL, run a file or load a mod. Anything
  unrecognised is refused whole rather than half-drawn, and no network at all
  simply means no board.

- **Infantry is a choice now, not a quantity.** Every soldier used to be the
  same soldier, so recruiting was one number and every battle was arithmetic on
  it. There are four kinds now — three of them infantry — and each answers
  differently in every column:

  | | money | manpower | frontage | attack | defence | fuel |
  |---|---|---|---|---|---|---|
  | Line Infantry | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 |
  | Militia | 0.50 | 0.60 | 1.00 | 0.70 | **1.15** | 0.80 |
  | Assault Infantry | 2.00 | 2.50 | **0.80** | **1.35** | 0.95 | 1.20 |
  | Mechanised | 3.50 | 4.00 | **0.60** | 1.25 | 1.15 | 3.00 |

  **Frontage is the column that changes the game.** A province fights across a
  limited width, and above it both sides' counts cancel — power reduces to
  `width x stat/frontage`, so the fight is decided by attack and defence PER
  METRE rather than by how many men arrived. Bringing more stopped being the
  only answer; bringing better now competes for the same manpower pool.

  Line Infantry is exactly 1.00 in every column on purpose. Every soldier in
  every existing save is line infantry, so the multipliers changed nothing at
  all until a second type existed — the first stage shipped as a byte-identical
  evaluation rather than a hope.

  **Militia was wrong once, and the mistake is worth keeping.** It was drafted
  at 1.20 frontage on the reasoning that an untrained rabble uses a frontage
  badly. That sounds right and is arithmetically fatal: it made militia worse
  than line at *both* attack and defence per metre, so it was strictly dominated
  in the quarter of all assaults that are width-bound. An AI rule buying the
  best defence per unit of money duly picked it and lost 34 and 28 rating points
  on two models, with the floor falling from 110 to 26. Frontage is footprint,
  not skill: a militiaman occupies a man's width like anybody else, and what he
  lacks is training, which is the attack column.

- **The AI stops cutting research first when money runs short.** Austerity took
  the research slider down before anything else, on the reasoning that a slider
  comes back up for free the moment income recovers. For a country that is merely
  tight rather than failing, that is the growth engine being switched off to pay a
  bill it could have met another way. Research now comes down last, after
  everything else has been given.

  Measured in the shipped binary with the shipped model, against its own absence,
  on two hold-out seed sets it was never tuned against. Six of six seats scored in
  every arm, no seeds lost:

  | | rating | survival | worst seat |
  |---|---|---|---|
  | hold-out C | 241 → 311 | 81 → 77 | 36 → 17 |
  | hold-out D | 255 → 297 | 100 → 80 | 105 → 38 |

  **Both halves of that replicate, and the second half is not a small-par
  artefact.** The four seats not under existential pressure gain between 2.3 and
  17.6 points of the world's land each. The seat being invaded loses ground on
  both sets — several points of raw land share on a seat whose par is 6.7, so
  real territory rather than a ratio effect on a sliver. On hold-out D the
  previous behaviour held every seat above par and this one does not.

  That loss is deliberately not quoted to the decimal. The invaded seat is
  bistable: across 22 recorded runs it either holds roughly 6 to 11 percent of
  the world or collapses below 1.5, with nothing in between, so a three-seed
  mean of it is nearer three coin flips than a measurement — it prints to two
  decimals and moves by a factor of three on the draw.

  Nor does playing both arms on the same worlds rescue it. Pairing controls how
  hard the world is; it does not control which side of the cliff a run lands on,
  and a seat this close to a tipping point can be flipped by a small change to
  the policy alone — the same seed has put two different models on opposite
  sides of it. What holds the direction up is replication: the sign is the same
  on two independent seed sets, and the rating gaps behind it (+71 and +42) sit
  well outside the noise floor of a three-seed run, which is about a dozen
  points of standard error. Expect a re-run on fresh seeds to reproduce the
  direction and not the number.

  Said plainly, because the rating alone would not say it: the AI expands harder
  and holds considerably more ground when it is not fighting for its life, and a
  country being overrun does worse than it did before.
  `OD_AUSTERITY_RESEARCH_LAST=0` restores the old order in one variable.

- **Publishing your books now matters to the AI, not just to migrants.** It
  read the true treasury, army, industry and ground of every country directly,
  so a country's decision to publish or withhold changed nothing about how the
  AI treated it. The relational feature that meant "their treasury" now means
  "their treasury if they publish it, otherwise a guess from their industry and
  their ground" — a country that keeps its books shut is genuinely harder for
  the AI to read.

  **This wants a retrain, and the shipped model has not had one.** The model
  file format is unchanged, so a policy trained before today loads and reads the
  new input as though nothing happened. Measured against that frozen policy the
  switch costs 2 rating points, with survival and worst seat unchanged, which is
  inside the noise floor — and the reason it is that small is that 11.1% of
  relational reads are fogged, because AI countries publish their treasury about
  73% of the time. `OD_AI_FOG_TREASURY_OFF` restores perfect information, which
  is what a training control arm wants.

  Two measurement knobs come with it, both default-off:
  `OD_AI_REL_ABLATE_TREASURY=<v>` replaces that feature with a constant, which
  bounds what the fog can cost (destroying it outright costs 5 rating points, so
  fogging one read in nine cannot cost more), and
  `OD_AI_REL_ABLATE_WHERE=fogged|published` destroys only one half of the reads,
  which is what can tell a policy that learned the mechanic from one that leaned
  on the guess — the unconditional version cannot, since both would lose more.

- **AI countries still do not spend on suppression, and now it is understood
  why.** The rule that funds it was documented as blocked by the AI's empty
  treasury. Measured: with the risk asked about the right government, the worst
  province anywhere in 203 reviews scores 10.14 against a bar of 12, so the rule
  could not fire for any country however rich. Lowering the bar and funding a
  floor out of gross income makes it fire (19 of 217 reviews, ~6% of income),
  and costs the player 4 rating points at bar 5 or 13 at bar 3 — but the world
  aggregate cannot say what the money buys at three maps, so both remain behind
  `OD_AI_PACIFY_BAR` and `OD_AI_PACIFY_GROSS` rather than becoming the default.

- **The AI was judging its own provinces by the player's unrest.** The reflex
  that cuts a country into districts and decides where its suppression money
  goes asked `getProvinceRebellionChance(pid)` — the one-argument form, which
  answers "how likely is this province to revolt against the PLAYER", because
  that is the only government the panels calling it ever ask about. Every AI
  country was therefore cutting its districts and aiming its money by a figure
  belonging to somebody else's government. It asks about the right one now.

- **Map scripts can see the game the game has become.** The language knew about
  treasuries, populations and borders, and nothing that has been added since:
  a script could not ask how many soldiers a country had, what it earned, what
  it was worth, or how it was governed. It can now — `country.X.troops` and
  `country.X.troops.militia` for the army by kind, `income`, `expenses`,
  `national_value` and `population` read from the same snapshot the economy
  screen draws, `district_count` and `district.N.share`, and on a province
  `troops`, `district` and `rebellion_chance`. Garrisons are writable per kind
  (`set province.42.troops.militia 12000`).

  Research groups can be forced open or shut — `set country.USA.research_groups
  1` holds a country to a single programme however rich it gets, `3` gives it
  three however poor, `0` hands the decision back to its economy. The override
  outranks the economic gate in both directions and is saved with the game, so
  a lock a turn-zero script sets is still there after a reload.

  New sugar: `wait 5 turns` (the target is worked out when the script parks, so
  `label / wait / jump` really does wait five turns every time round rather than
  racing past a date that has gone), `set x to <expr>`, `{value}` interpolation
  inside `print`, and `foreach district in country.X`. Everything above has a
  block in the editor's palette — which now scrolls, having outgrown its own
  panel at seventeen entries.

- **A map's scripts no longer depend on its metadata being honest.** The loader
  scanned the archive for `scripts/` entries and then let `metadata.json`
  overrule what it found, so a map whose metadata was written before its scripts
  were added carried scripts that silently never ran. Metadata can now only turn
  scripting on, never off.

- **Districts are authorable in the map editor.** The Countries tab gains a
  Districts brush beside the claims one: add districts, name them, colour them,
  set each one's claim on the pacification budget, and drag over provinces to
  assign them. A district is a partition rather than an overlay, so painting a
  province into one takes it out of the one that held it, and a legend on the
  map says how much of the country is still in no district at all. It travels
  in the map as `districts.json`, and a country the author divided starts the
  game divided that way — laws included.

  The AI treats an authored division as scenario content: it will not redraw a
  country somebody cut up by hand, and decides only where that country's money
  goes, bounded so no district the author drew is starved to nothing. It still
  cuts its own districts freely for every country nobody drew.

- **The editor's bottom bar no longer prints its labels through its buttons.**
  It was laid out on fixed pixel offsets measured against English: "Tools" is
  33px wide, "Інструменти" is 96, and in half the languages the game ships the
  first tool button sat on top of the label — as did the brush-size number on
  the slider. It now measures the labels and lays out from them, and the tool
  names themselves are translated.

- **Country profiles.** Any country's province panel now opens a profile of the
  country that holds it: how long it has existed (in months, years or millennia,
  or "existing since the start of the session" for a country that was on the map
  when the game began), its population and where the graph has been going, its
  gross income and its expenses, where it sits on the political compass, and the
  flags it has flown, in order, with the turn each was raised.

  Three further figures are the country's own to publish or withhold — the
  composition of its expenses, the doctrines it has in force, and the treasury it
  held at the start of last turn. Publishing is not free advertising and not
  merely flavour: migrants read it. A country whose disclosed figures are good
  draws people to it, up to a fifth again on the attractiveness a province
  already had, so an honest, wealthy, calm country fills up faster than a
  secretive one — and a country with bad figures is better off saying nothing.
  The AI publishes and withholds on the same terms you do.

- **New territory joins the nearest district**, not the first in the list. A
  province it borders wins outright; otherwise the closest one, so an island or
  a landing across a strait still lands somewhere sensible.

- **Districts can be removed**, down to the last one, and their ground rejoins
  the nearest surviving district. A district that loses all its provinces stops
  existing.

- **Districts name themselves after the ground they hold.** A people holding a
  real majority lends its name — "Mongol County", "Norwegian County" — and
  otherwise the district is named for where it sits in the country: Northern,
  Eastern, Central. The word for a region is a property of the *map*, chosen
  from its own content, so one world has counties and another has oblasts
  rather than mixing them.

- **Every doctrine that says it reduces unrest now does.** The value was stored
  as a fraction and displayed as a percentage, but the resolver subtracted the
  fraction straight from a figure measured in percentage points — so Secret
  Police advertised "5%" and delivered 0.05, against a loyalty floor of 6. Every
  such doctrine in the game did nothing at all. Rebellions per 1k country-turns
  fall from 81 to 30 on the 1939 map and 77 to 30 on 1914, and the seat
  benchmark goes from 72 to 121 with survival 49 to 87.

- **Districts pass regional law**, which is its own body of content rather than
  the national doctrine list under a smaller heading. Ten of them: a curfew, a
  tax holiday, language rights in the courts, martial law, company towns, grain
  requisition. Each is a trade — calm bought with money, or money bought with
  resentment — and several make things worse on purpose. They are priced *per
  province*, so the same law is a different bill in a big district than in a
  small one, and their effects on unrest, income and population growth apply
  only to the ground that district holds.

- **Districts: a new Politics tab where you divide the country up.** Pacification
  used to be one slider for a whole country — the same weight on a quiet county
  as on the one that is arming, so you paid for the calm half in order to reach
  the loud one. Now you can cut the country into as many districts as you like:
  paint provinces into them on an inline map, colour them, and give each a share
  of the budget. The shares always add to 100, and a button splits them evenly.
  The pacification budget itself has moved to this tab.

  What matters is a district's share of the **money** against its share of the
  **ground**. One holding a tenth of the country on a tenth of the budget is
  policed exactly as before — so dividing changes nothing until the shares are
  uneven, and an undivided country plays the game it always played. Each row
  shows the multiplier it comes to.

- **AI countries draw their own districts**, splitting the worst fifth of their
  provinces off from the rest and aiming the budget at the trouble.

- **The unrest tutorial covers it**, in all twenty languages.

- **A new game is a new world.** The world seed only ever reached the turn
  resolver, so two new games on the same map started from an identical position
  — same countries, same politics, same flags — and differed only in what
  happened afterwards. A fresh world now nudges each country's political compass
  by up to 18 points of 200, which is enough to carry the ones sitting near a
  threshold across it and shows up as different ideologies, names and flags. The
  same seed still reproduces the same world exactly.

- **Fixed the black screen after Process Turn.** Regenerating the political map
  is seconds of work and had been moved out of the turn's loading screen into
  the draw loop, where nothing was on screen while it ran. It happens inside the
  turn again, behind its progress bar.

- **Orders are shown in the view they belong to.** Marches and artillery in Army
  Navigation, voyages and naval guns in Navy — matching the recruitment and
  construction marks, which already worked that way. The banner says which view
  carries what.

- **Fixed the claims tabs overlapping.** They sat on a fixed 140px pitch whatever
  they said, so any language whose words are longer than English's ran one label
  into the next.

- **The game renders at the display's real pixel density.** Until now the
  framebuffer was the window's logical size and the system stretched it to the
  panel, so on a 2x display every pixel of the map, the interface and the text
  was drawn once and shown as four. That is what "blurry on a big display" was,
  and the font was only the part that showed it first. Set `"highDpi": false` in
  config.json to go back.

- **Text is sharper in every script that can afford it.** The glyph atlas was
  rasterised at 16 pixels and scaled up for every size drawn, which on a large
  display is a 16-pixel bitmap smeared to fit. Cyrillic, Latin, Greek, Arabic,
  Devanagari and Turkish are now rasterised at 32. Japanese, Chinese and Korean
  have several times as many glyphs and stay at 16, so that no atlas is ever
  larger than the biggest one the game already built — nothing asks a machine
  for memory it was not already asked for.

- **A nation can be freed as part of a settlement.** Releasing was a button you
  pressed on yourself and nobody could ever ask you to — which is the shape of
  most real peace treaties. It is now a term like any other in a ceasefire or a
  trade: offer to free one of your own peoples, or demand the other side frees
  one of theirs. Received offers name it plainly, and the offer map draws the
  ground in its own colour, because land leaving for a *new* country is not the
  same event as land changing hands between the two of you.

- **You choose which territories a freed nation gets.** Releasing used to hand
  over the whole region a people held, all or nothing. Now pressing Release
  opens the choice: the region is marked on the map, you keep or give each
  province, and the game says why a selection will not do — too small, or in two
  pieces. A country in two halves with someone else's ground between them is not
  a border anybody drew.

- **The orders view shows naval bombardment.** A carrier group working over a
  coast was the one kind of attack it could not draw, so a shelled shoreline
  looked like a quiet turn. Shown as a shell's flight from the hull, with a mark
  at the firing end so it reads apart from a battery inland.

- **All twenty languages are complete again.** The turn phase, research groups,
  troop kinds and economy graphs are translated, along with a set of older map
  editor and goods strings that had never been done — 86 strings, 1507/1507 in
  every language.

- **Research groups are cards, not a strip.** Each one now shows what it is
  building, a progress bar for it, its share of the budget and its Auto switch,
  side by side so you can compare the three at a glance. They were 21px rows: a
  name clipped to fit, no progress at all, a slider the width of a thumbnail —
  the primary control of a whole subsystem drawn as the smallest thing on the
  screen. On a narrow canvas they drop to a row of their own rather than off
  the edge.

- **"Show orders" is now a pause after the turn, not a layer over play.** It
  used to paint every order in the world onto the live map while you were still
  giving orders — a labelled box on every province on Earth, on top of the
  troop counts already there. It now means "stop after each turn and show me
  what everyone did", which is the same setting the phase's own "Skip this from
  now on" writes, and it works from the first turn.

- **Orders use the map's own symbols.** A levy, a works going up, a hull on the
  slipway and a specialisation are marked with the same glyphs your own pending
  orders have always worn, in the same view each belongs to — so the industry
  map is not also an army map. Tinted by country, so whose order it is stays
  readable when four countries are building along one border. Standing orders
  appear once you look at a region rather than at world zoom, and the banner
  says how many are waiting there.

- **Research budgets add up to 100%.** The three group shares were each
  independently 0–100, so two groups could both read 90% and the panel showed a
  country spending 180% of its research. Nothing was overspent — the points were
  normalised before being paid out — which is worse, because the numbers on
  screen then meant nothing. Moving one share now pushes the difference onto the
  others in proportion to what they hold. AI countries run the same rule.

- **AI countries run research groups too**, on the same unlock rule as you.
  Measured: research completions per 1k country-turns rise from 7.4 to 11.3 on
  the 1939 map and 10.3 to 15.4 on 1914.

- **Assault Infantry now follows Militia Levies** instead of Combined Arms, so
  the Formations tree is a ladder of its own rather than three nodes whose
  prerequisites live in another tree.

- **The economy screen shows what the country is worth.** Two new graphs: income
  against expenses with the zero line drawn, and national value — everything
  built and fielded priced from the same tables the build buttons charge from,
  plus a per-head figure. The old five-line income graph flattened net income
  into the baseline and put its legend underneath the pie charts.

- **Fixed the two pie charts overlapping.** The second advanced 100px from the
  first one's *title*, but the first is 110px of circle plus a legend that runs
  further, so any country with more than about four kinds of expense had its
  charts drawn on top of each other.

- **Research runs in up to three groups.** Each has its own project, its own
  share of the budget and its own Auto switch, so a country no longer queues
  every technology in the game behind one. How many you get is decided by the
  size of your economy against the rest of the world — a second group at twice
  the world's median, a third at five times — and then gated by income per
  head: an economy spread too thin over too many people supports one programme
  however large it is in total. One is always available whatever your position,
  and a locked group tells you which of the two is holding it back. On the 1939
  map the great powers all run three; China, with the fifth largest economy on
  the map and a fifth of the world's income per head, runs one.

- **A research group can follow its branch on its own.** Turn Auto on and it
  starts the next technology as soon as the last one finishes, stopping the
  moment the branch offers a real choice — two nodes open at once — or runs out.
  It will never pick which branch to start; that is the judgement it exists to
  hand back to you.

- **Formations and Efficiency are their own research trees.** Both were extra
  columns bolted onto trees that were already full, with prerequisite lines
  running the whole width of the canvas across two other branches to reach
  them. A prerequisite in another tree now draws no line and is named in the
  tooltip instead, with the tree it lives in.

- **A province can raise more than one kind of soldier a turn.** Each kind
  keeps its own slider and its own order, all drawing on the one population, and
  a kind already on order is marked on its tab. Queueing militia used to lock
  the province for the turn.

- **Fixed the recruitment ceiling reading a hundred times too high.** Garrison
  counts are stored a hundred to the soldier and the readout was not dividing,
  so it offered "82% of 1.08m" beside a button raising 8.9k men. The button was
  right all along.

- **Ships no longer sail across land.** Sea routes were planned on a grid where
  a cell counted as navigable if it held *any* water, so two cells on opposite
  shores of a peninsula were treated as neighbours and the leg between them ran
  overland. Russian hulls crossed Crimea; the same fault sent fleets over
  Jutland, over Italy and across the base of Florida. Every leg is now checked
  against the map before the router may use it. Measured on the shipped world,
  the worst stretch of dry land under a route falls from 53 pixels — about 260
  km — to 16, which is the width the game deliberately treats as an undrawn
  strait so the Bosphorus and the Dardanelles stay open.

- **The land no longer changes while you are reading the orders.** The Viewing
  Orders screen showed the map as it was *after* the turn resolved, so borders
  had already moved to their new owners while you were still being shown the
  attacks that moved them. The political map now holds still until you press
  Continue. As a side effect a turn that ends several wars regenerates the map
  once instead of once per ceasefire.

- **The orders view says what countries are building and raising**, not only
  where their armies went — recruitment and construction now appear on the
  province they are happening in.

- **Army orders are drawn as army arrows**, weighted and filled like the ones in
  the army view, and each carries the share of the garrison that marched. The
  draggable knob is gone from this view: it was a handle on an order that had
  already resolved.

- **The recruitment slider shows what its ceiling actually is.** The cap has
  always been counted in *people*, so a province that raises 120k line infantry
  raises only 30k mechanised — true from the day the troop types landed, and
  invisible, because the readout showed a percentage and nothing else. It now
  reads "50% of 1.65b", and the figure moves when you change the kind.

- **Show orders no longer takes your sidebar away.** Suppressing the tabs is
  right for the Viewing Orders phase, where no order can be given; it was wrong
  for the toggle, which is a lens held over ordinary play.

- **Fixed the army research tree drawing through the navy column.** The new
  Formations branch was placed at the far right of the canvas, so its
  prerequisite lines ran the full width of the tree and across two other
  branches. It now sits beside Army, where everything it depends on is.

- **The turn now has a middle.** When a turn resolves, the game pauses on a
  "Viewing Orders" screen showing what every country actually did — the marches,
  the bombardments, the voyages — before handing the map back to you. Nothing
  takes orders while it is up; a Continue button moves you on, and "Skip this
  from now on" turns it off for good if you would rather not have the beat.
  You can still open the same view any time from the Show orders option under
  Process Turn.

- **Your army can be made of different things.** A new Formations branch in the
  army research tree unlocks militia, assault infantry and mechanised troops.
  Each is a different bargain: militia are cheap in money and in men and hold
  ground well but crowd a battle line; mechanised cost four times the manpower
  per soldier and drink fuel, but use a frontage better than anything else.
  Pick which kind a province raises with the new selector above the Recruit
  button — and because a better soldier represents more of your population, the
  same province raises far fewer of them.

- **You can see what is standing in a province, and order it about by kind.**
  The army view listed one line per stack and ran off the bottom of the panel if
  there were many. It now shows your garrison broken down by type, then anybody
  else's troops by country — scrollable, and sized to what is actually there.
  Click a type to aim your next order at just those troops; click "All our
  troops" to go back to commanding the province as a whole. On the map, a
  province holding more than one kind of soldier gets a small bar under its
  marker showing the mix.

- **Groundwork: an army is made of something.** Soldiers now have a kind. Nothing
  in this release changes — every soldier in every existing campaign is line
  infantry and behaves exactly as before, and a save written now is byte-for-byte
  what the previous version would have written. What it buys is everything that
  comes next: different troops, at different costs, drawing on a limited pool of
  manpower. Old saves load correctly, and a save from this version can still be
  read by an older one, which will see the right number of men.

- **A guarantee is worth something in the war you signed it for.** Guarantees
  only ever fired at the moment war was declared, so a country that won one on
  turn three of a war it was losing got nothing from it — the pact was dead
  paper for the only war it was obviously signed for. A guarantor can now be
  called to arms like an ally, and can accept or refuse. And calling for help is
  no longer something only the human player could do: every country can now ask
  its friends to join a war already under way, which is the point of having
  friends.

- **A beachhead is supplied by the fleet that put it there — while the fleet is
  still there.** Troops landed on a hostile shore have no road home, which the
  supply rules would otherwise read as being encircled. A friendly or allied
  fleet within landing range now supplies them. Lose that fleet, or sail it
  away, and the beachhead is genuinely stranded and fights like it. Landing
  under cover of a navy and then leaving is no longer free.

- **Battles now last, and you can feed them or pull out.** An attack used to be
  over the moment you ordered it: it took the province or it was thrown back and
  the survivors walked home by themselves. Now an attack that neither wins nor
  dies **stands in the province** and fights on, a round each turn. Send more
  troops to that province and they join the fight instead of starting a separate
  one; or order a withdrawal from the province panel and pull your men back to
  where they came from. Retreating used to be free and automatic. It is a
  decision now — and so is committing.
  A failed landing still drowns: there is nowhere for it to fall back to.
  Your army in a battle still eats, still draws ammunition, and still costs
  upkeep, because a war has to be paid for while it is being fought.

- **Armies far from home fight worse, and cut-off armies fight badly.** Nothing
  in the game knew how far an attacker had marched: a stack twenty provinces
  deep fought exactly as well as one defending its own capital, so pushing deep
  cost nothing and defence in depth had no point beyond stacking forts. Supply
  is now measured from your ports and your largest industrial province, along
  roads through your own and your allies' territory. Two provinces out costs
  nothing; beyond that your troops fight progressively worse; and a province
  with **no land route home at all** is cut off and fights badly. Severing a
  corridor is now a real operation with a real payoff — and so is holding one
  open. A landing is supplied from the sea rather than treated as encircled.

- **Bringing more men to a battle matters again.** Combat width caps how many
  troops either side can bring to bear on a province — but it capped *both*
  sides, so once two armies both filled the frontage the fight was decided by
  research bonuses alone and returned the same answer every turn no matter how
  many men you sent. An attack with 174,800 men against 93,048 was repulsed three
  times running, each time losing exactly the same number of soldiers, because
  the extra 139,000 counted for nothing. Men beyond the frontage are now the
  reserve: they can't widen the fight, but they feed it, so a deeper army fights
  harder. A narrow fortified pass still stops numbers — that is what the frontage
  is for — but on open ground, weight of numbers tells.

- **A doctrine that lets you fight several countries at once.** You could only
  declare one war a turn, and the reason written in the code only ever justified
  the narrower rule it sat next to (you still cannot say two things to the same
  country in one turn, and that has not changed). *War on Several Fronts* raises
  it to three: an authoritarian doctrine at 16/turn over six turns, paid for with
  a fifth more army upkeep, weaker defence in the field and more unrest at home.
  The old limit also lived in the diplomacy panel rather than in the rules, so it
  bound you and not the countries you were playing against — it is one rule now,
  and everyone is held to it.

- **The mobilisation doctrines now actually work.** How a country raises its
  army was supposed to change what the army costs to recruit and to keep —
  conscription cheap to raise and dear to maintain, a professional army the
  other way round. That was written, documented, and never reached a single
  game: the shipped maps carry their own copy of the doctrine data, and it had
  not been updated. Fourteen doctrines now charge the army upkeep and
  recruitment costs they had been advertising all along, so armies are more
  expensive than they were and a large one is a real commitment.

- **Going broke for one turn no longer breaks your country.** An empty treasury
  added a flat, very large amount of unrest to every province from the first
  turn it happened — the same charge whether you had been insolvent for one turn
  or twenty. It now builds over three turns: a third, two thirds, then the full
  weight. Chronic bankruptcy is exactly as punishing as it always was; what has
  gone is the case where an invader takes your industry and the resulting
  one-turn cash shock fractures an otherwise stable country before any
  government could have cut a budget. Measured on three worlds: 3.8 to 5.7 more
  countries in a hundred survive, and rebellions fall by 17–30%.

- **You can see where your ships are actually going.** A pending naval move drew
  a straight line to the destination — which is the one route no ship ever
  sails, because they steer around land. The map now shows the real path, with
  the stretch your ship covers *this* turn drawn solid and the rest of the
  voyage faint, a dot where it will stop tonight and a ring on the destination.
  A destination with no sea route to it is marked with a red cross instead of a
  line, rather than looking like a very long voyage.

- **A new Orders view shows what everyone did this turn.** Toggle it from the
  button above Find country: artillery, army movements and naval voyages for
  every country, each in its own colours, drawn over whichever map view you were
  already in. Politics, Economy and Research step out of the way while it is on
  — it is a view for reading the board, not changing it — while Find country,
  Settings and Claims stay put. It works in multiplayer, where the host sends
  each turn's orders to everyone; an older client simply does not show the
  overlay. It shows the turn that has *resolved*, never the orders other players
  have not yet committed.

- **Every new game is now its own world.** Starting a new game drew from the
  same fixed random sequence every time, so the same rebellions arrived in the
  same provinces on the same turns in every campaign anybody ever played. New
  games now take a genuine random seed, and the load log records it — if a
  world does something worth showing somebody, the number that reproduces it is
  in your log. Saving and reloading continues the world's luck from where you
  left it rather than rewinding it, so reloading is not a way to reroll a bad
  turn. Set `OD_WORLD_SEED` to pin a world deliberately; the AI benchmarks pin
  their own and are unaffected.

- **A province will only carry the industry its land can support.** Level 10
  used to be available anywhere on the map, so an uninhabited island with no
  resources could out-produce a city. Every province now states its own ceiling
  in the province panel — "Level II of VI", and it says so when it is full —
  set by how many people live there, how tightly they live, how much ground it
  covers, and what is buried in it. A factory in an empty province really is an
  expensive shed now, which is what the tutorial has been telling you all along.
- **What a factory earns depends on where it stands.** Industry income was a
  flat rate per level everywhere. A factory built well beyond what its province
  can support now earns proportionally less, and one built in a province that
  can carry it earns exactly what it always did.
- **Provinces you have already overbuilt keep everything they have.** The new
  ceiling applies to what you build next, not to what you built before. An
  existing factory above its province's ceiling is left standing and says so in
  the panel — you will see a handful of these on the shipped maps, in the
  historically industrial parts of France and Belgium.

- **Your population no longer runs away.** It was growing 52-fold over a long
  campaign — two billion people at the start of the 1939 map became a hundred
  and ten billion — because population growth was being applied once per ethnic
  group in a province instead of once per province, and the ceiling meant to
  stop it was set 133 times higher than the largest province that has ever
  existed. Growth is now applied once per province, ethnic policies are weighted
  by each group's share of it, and the ceiling is the ground a province actually
  covers.
- **Mobilisation doctrines change what an army costs.** Raising one and keeping
  one are now opposed: Mass Mobilisation makes recruits 30% cheaper and upkeep
  25% dearer, Demobilisation the other way round, a Professional Army costs more
  both ways and fights far better. Seventeen doctrines carry these now, and the
  economy panel shows the rate you are paying.
- **Army maintenance discounts finally do something.** Eight doctrines and
  research nodes have advertised an army upkeep reduction — Demobilisation says
  -25%, Austerity -20% — since the day they were written, and not one of them
  was ever applied to anything.

- **You can let a nation go.** Where a disaffected people holds the majority
  across a run of your provinces, you can grant them independence: they leave as
  a country at peace, under your guarantee. You lose the land, the people and
  the industry; you lose the unrest with them, and gain a friend. Repressing a
  minority is what makes their region releasable, and reconciling them is what
  removes the need — so this is the last rung of the ethnic-policy ladder rather
  than a lever of its own.
- **A country that cannot pay for itself for years on end will shed a region**
  rather than disintegrate. It is the final step of going broke, after budgets,
  doctrines, minority programmes, ships and troops — and the only one that
  cannot be undone.
- **A province can only hold so wide a front.** Numbers stop being decisive past
  what the ground will take: a huge army no longer automatically rolls over a
  well-sited smaller one, and a fort now narrows the front as well as
  strengthening the defenders. Wide provinces let more of an army bear, narrow
  ones are passes.
- **A failed attack no longer destroys your whole army.** The men who fought are
  lost; the ones still in reserve fall back to the province they came from. A
  failed amphibious *landing* still drowns — there is no ground behind it.

### Fixes

- **Ships stop sailing the wrong way round the world.** Near the date line a
  one-degree hop measured as three hundred and fifty-nine, so a fleet in the
  Aleutians would spend its whole turn steaming east across the Pacific instead
  of taking one step west, and the route preview quoted twenty turns for a
  two-turn crossing. The router always knew the way; nothing that measured or
  followed it did. Ship moves stopped dead by land fell from roughly a third to
  under a tenth.
- **The move preview no longer draws a route that does not exist.** When the
  router could not find a path it drew a single straight line to the target,
  across whatever land lay between. Unreachable destinations are now shown as
  unreachable.
- **A conquered country's claims die with it.** Destroy a neighbour and every
  claim it had ever pressed stayed on the board for the rest of the game,
  agitating your provinces — visibly, and with no way to answer them, since the
  country that could drop them no longer existed. On a long game two thirds of
  all claims belonged to the dead. Rebellions fell by well over half.
- **"Declare War" tells you why it is greyed.** You may only declare one war a
  turn, and the button did not know it: after your first declaration every other
  one accepted the click, did nothing, and said nothing. You may still only
  declare one, but now the game says so.
- **Splitting a garrison no longer strands a man behind.** Dividing a province's
  army in two rounded both halves down, so an odd garrison left a single soldier
  standing — an army marker on the map reading "<1" after you had moved
  everybody out. Half of all garrisons are odd, so it happened about every other
  time you split one. Shares are now taken so they add up to the whole.
- **Orders die with the ground they were given for.** A disband or move order on
  a province you then lost survived, and fired if you ever took the province
  back — an army recaptures a province and immediately disbands itself for a
  decision made before the province changed hands twice.
- **The disband marker names the number.** It said "Disbanding..." and nothing
  else, while the order means "everything here when the turn resolves" — so
  troops moved into a marked province went with it, invisibly. It now shows what
  will actually go.
- **You can search your Implementing and Active doctrines**, not only the ones
  you have yet to enact. The search reads descriptions and effects as well as
  names, so "upkeep" or "navy" finds the doctrines that do those things.

### Experimental, off by default: the production economy

Set `OD_GOODS=1` to play a world where factories make **things** instead of
money. Not finished, not balanced, and not on unless you ask for it — a world
started without it plays exactly as it did before, to the number.

- **Factories produce goods.** Consumer goods, machinery, fuel and munitions,
  made from the oil, metal, rubber and gemstones your provinces hold. Gold is
  still money.
- **Your population eats.** Consumer goods feed it, and how well fed it is shows
  in the economy panel as living standards. Fall short and unrest rises
  everywhere at once until you fix it — build more, take somewhere that can, or
  trade for what you lack.
- **Undirected factories direct themselves**, making whatever the country is
  shortest of and can actually build. Choosing for yourself — the planned
  economy, and the politics of who decides — is not in yet.
- **`OD_AUTOSELL_PCT`** sets how much of your surplus raw material sells itself
  each turn: `100` (the default) behaves much like the old economy, `0` means
  nothing sells unless you trade it, and the interesting settings are between.

**Armies now cost things, not just money.** Recruits need munitions, artillery
and naval bombardment need fuel and munitions, and a standing army burns fuel
every turn — if you have none, you buy it in at a painful price, and the economy
panel names that bill. Your mobilisation doctrines set both exchange rates: the
one that makes recruits cheaper makes their munitions cheaper too, and the one
that cuts army upkeep cuts its fuel. So machinery, fuel and munitions are worth
making at last, and a large standing army is a burden on your factories rather
than only on your treasury.

Known gaps: national stockpiles are not saved yet (they reset when you load;
factory assignments do persist), armies and artillery still cost only money,
and the production rates have been tuned by hand rather than measured.

## game 1.1.2a

- **The browser version no longer dies when you process a turn.** It was
  overflowing the WebAssembly stack on the very first turn — every scenario,
  every country, spectator included.
- **Turns stop getting slower the longer you play.** The save was rewritten and
  recompressed whole every turn: 4.5 seconds a turn by turn 150, now under one
  and flat. Quitting mid-save can no longer destroy the save either.
- **The map tells the truth again.** Conquered provinces change colour, the
  world behind a dialog is the right way up, and the mouse keeps working after
  you touch the screen.
- **Things you can reach.** Settings has a button in the sidebar, the country
  finder has a way out, and UI Scale and colour-blind mode answer a click
  instead of only the arrow keys.
- **The AI stops signing away its country** for a trade that returns nothing. A
  ceasefire may still cede ground — ending a losing war is worth paying for.

## game 1.1.1a

- **Processing a turn no longer blacks out the screen.** The world stays
  visible behind the popup while the turn resolves.
- **Armies stay out of countries at peace.** The rule the land march obeys now
  covers amphibious landings too, which were the way round it.
- **The AI stops spending itself into revolt.** It was placating its minorities
  without limit — a bill nothing ever scaled down — until it went bankrupt, cut
  every settlement at once, and faced the rebellions that followed. Two fifths
  fewer bankruptcies, a quarter fewer breakaway states, and it plays
  measurably better for it.

## Licence

Nothing to install: this changed in the repository, between releases, and
applies to anyone opening a pull request.

**The licence is version 1.1, and contributions are assigned rather than
licensed.** Clause 4 used to say a contributor keeps the copyright in their
patch; it now points at [CLA.md](CLA.md), which assigns it to the project —
with a licence back, so a contributor keeps the right to use their own work
anywhere else, commercially included. The reason is that clause 5 lets the
licence change later, and that power reaches only rights the project holds.

Playing is unaffected, and so are mods, maps, saves and videos: clause 3 still
says those are yours, and the CLA says it again. Every commit in the repository
predates this, which is the only moment such a thing can arrive without asking
somebody to re-sign for work they already gave.

## game 1.1.0a

- **Twenty-one languages** — interface, country names and dialogue. Arabic,
  Hindi and Urdu are properly shaped, not drawn letter by letter.
- **Advisors and officers speak to you**, drawn in your country's own colours.
- **A tutorial**, with a map built for it.
- **The AI holds its ground when attacked.** Recruits come out of a province's
  population, its action cap is gone (requests are limited instead),
  coalitions no longer form on the hardest difficulty, and it fortifies a
  threatened border. Where the last build was reduced to almost nothing in a
  world that attacks without pause, this one keeps its starting share — rules
  and trained model both changed, so that is the two together.
- **A score you can measure yourself against.** `tools/od_bench.py` plays six
  fixed seats; 100 means a seat kept its ground. The AI scores 107: strong in
  a peaceful world (Sweden, France 200), weak in a hostile one (China 8).
- **Map scripting, version 2.** Conditions are real expressions now —
  arithmetic, `and`/`or`, parentheses, `min`/`max`/`clamp` — where before a
  condition was one comparison and nothing else. New: `elseif`, `unless`,
  `for i = 1 to N`, `repeat N`, `break`, `continue`, `print`, and assignment
  the C way — `x++`, `x--`, `x += 1`, with `set` optional. Version 1 scripts
  run unchanged. Fixed: an `if`
  with an `else` ran both halves.
- **A map can bring its own characters.** `comms/cast.json`, portraits and
  `.oddlg` dialogue inside the .odmap, played with `dialog <name>` — the same
  speaking window, live eyes and accent colouring the tutorial uses, scoped to
  the map that carries them.
- **Fixed: map scripts never ran.** Starting a new game takes the asynchronous
  loading path, and only the synchronous one ever started the script engine —
  so a map's scripts did nothing in ordinary play.
- **Scripts can find a country without knowing its code.** A generated world
  hands out codes the mapmaker never sees, so `foreach country in world`,
  `country.of_province.42`, `country.largest` and `country.player` name one by
  what it is instead. Maps can also switch rules off:
  `set rules.rebellions false`.
- **Scripts can ask what mods are installed.** `if mod.com.example.extra`, and
  `.version`/`.name` alongside it, so a map can offer something extra without
  requiring it. Also `try`/`catch`, `label`/`jump`/`stop`, and `spawn` for
  running several flows in one script — cooperative, not threaded, so it
  behaves the same in a browser.
- **Scripts can be edited as blocks.** A Text/Blocks toggle in the map
  editor's script IDE: drag statements around, add them from a palette, and
  switch back. The two views are the same file: statements, comments and blank
  lines all survive the trip, and only decorative indentation is re-aligned to
  the nesting the blocks show.
- **Greater Diplomacy 5 translation.** Exporting a map in the browser no
  longer loses large worlds: the download link was being released in the same
  tick it was clicked, before the browser had finished reading it.
- Stability and assorted bug fixes.

## game 1.0.8a

- **Trade deals.** "Propose Trade" sits in the peacetime diplomacy list:
  provinces, money and claims moving both ways by agreement. A ceasefire
  without the war. The AI proposes them too, and prices land at what it earns
  rather than by counting provinces.
- **Flags are recoloured, not replaced.** A country whose politics move keeps
  its own flag, shifted toward the palette its politics imply, so reverting is
  exact. Seven symbols that shipped with nothing able to name them — anchor,
  torch, rose, fasces, cross pattée, four-pointed star, star of David — can be
  used now, and one device means a government where several mean a union.
  Previously every ideology reached for the same star.
- **Rebellion names.** 18.5% of generated breakaway names were malformed —
  "Mestizo Mexicia", "Han Chin". They are named after places now.
- Countries rename and restyle themselves as their politics move.
- **Quick Start** on the main menu puts you straight into a turn.
- Ceasefire offers can be any amount you actually have, including nothing.
- **Experimental:** translate maps to and from
  [Greater Diplomacy 5](https://github.com/GitGetGot415/Greater-Diplomacy-5).
  Off by default; Settings → Experimental. Lossy, and it says what it lost.
- Fixed: a way out of the multiplayer screen that is not Escape, Research
  greyed for spectators, and loading a scenario no longer asks a phone for
  1.7 GB.
- **Smaller, byte for byte the same game.** The download is 58 MB where it was
  62, and an installed copy is 9.2 MB lighter. The trained AI model is nine
  megabytes of float weights, and a plain deflate pass barely dents them:
  consecutive weights share a sign and an exponent byte and share nothing at
  all in the low mantissa — separate the four byte positions into their own
  streams first and it is 4.0 MB. The worlds, the flags, the app icon and the
  screenshots were each letting whatever wrote them pick a PNG scanline filter
  by a rule meant for photographs; flat art wants no filter, and the province
  layer alone was paying 18% for the wrong guess. The soundtrack, two thirds
  of the download, gave up 4.3 MB to OptiVorbis once a two-line bug in
  stb_vorbis was fixed: it read a zero-size allocation as a failed one, and so
  refused every optimised file. The zip is now built with zopfli, an ordinary
  zip found by searching harder for it, and the linker drops the code nothing
  reaches. Every map, every flag, every screenshot and every sample is exactly
  what it was: the encoders decode what they are about to write and compare it
  before it replaces anything.
- Windows builds in about half the time.

Not yet: the dedicated server has no downloads, and long-form turns are built
but never played across a real campaign.

## game 1.0.7a

The browser build is a ninth of the size it was, the borders are surveyed
rather than traced, armies have a button, and there is a dedicated server.

## The browser build

It used to download 69 MB before the menu drew, behind a canvas that stayed
black for all of it. It is now 12 MB, and it tells you what it is doing while
it works.

The six scenarios and the trained AI model are no longer in that download. None
of them is read until a player has picked a world, so preloading all six meant
waiting for five worlds nobody asked for; they are fetched when something asks
for them instead. The font was 11 MB of Unifont to draw about six hundred
characters, and is now a 147 KB subset of exactly those. The menu no longer
opens a scenario archive to find its own background.

**Settings and saves survive the tab now.** The web build's data lived in the
page and nowhere else, so closing it, reloading it, or letting the browser
reclaim it took the config, every save, every custom map and every installed
mod with it -- and said nothing, because every write had succeeded. They are
kept in the browser's own storage and restored on the next visit.

## The map

The borders of eleven countries and regions are now cut from OpenHistoricalMap's
surveyed outlines instead of being traced by hand: the German-Polish frontier,
Austria-Hungary, Finland, Hungary, Turkey, Asia, South America, Bhutan, Ecuador
in 1939, the inner-German border and Luxembourg.

The archives are also a quarter of their old size -- 32.6 MB down to 7.6 MB --
by encoding the layers as indexed images rather than truecolour. They decode to
the same pixels; a land/sea layer answers one question per pixel and was being
stored as four bytes of it.

The flag artwork went from 12 MB to 5.7 MB the same way, and three flags that
were drawing wrong are fixed. Belize, Bhutan and the Kingdom of Serbia carried
their fills in a stylesheet the renderer does not apply, so Serbia drew as a
black field instead of a tricolour and Belize drew a black disc where its arms
should be.

## Playing

**Armies have a button.** Moving one meant holding the army-move key and
dragging, which is discoverable only by reading the keybinds, and players
reasonably concluded armies could not be moved at all. The province panel now
has a Move Army button; the label carries the keybind too, so the faster way is
learned from the slower one rather than instead of it.

**Disband All and Scrap All**, with the order counts and a way to cancel them.
Bulk upgrade and bulk specialise for provinces. Resource income is shown per
province, with the specialisation boost broken out.

**Population growth is a rule of its own.** It used to be a side effect of the
default deportation policy, which reached only provinces that had a minority
and scaled with how many -- so an ethnically homogeneous province never grew at
all, and a three-minority province grew three times as fast as its neighbour.
Every province now grows once a turn, at a rate research modifies rather than
provides.

Diplomacy refuses what it used to accept twice: an offer already awaiting an
answer, a second round of talks in one turn, a declaration already queued.

## The AI

It was being punished for making peace. A ceasefire that landed cost the war
module half a point on top of the reward for the peace itself, and a ceasefire
that was refused cost it half a point for nothing. It was also charged for its
own conquests, so keeping what it had taken read as a loss. Both are fixed, and
the model shipped here is the one worker from an overnight pool that beat its
own starting point.

## The dedicated server

A console server that needs no graphics card, no display and no X11 -- it
compiles the same simulation as the game against a raylib of its own, so the
two cannot disagree about the rules, and the binary links nothing that draws.

**It is released separately, on its own tag and its own schedule.** A VPS
operator should not download a few hundred megabytes of artwork to run
something that never draws a pixel.

## Fixed

**The Discord and GitHub buttons did nothing** on Windows, Linux and in a
browser. They ran a macOS command, so the only route from the game to its
community worked on one platform and failed silently on the three where nearly
every player is.

**The main menu overlapped itself** on any window shorter than about 790 pixels
-- 720p, a laptop with browser chrome, the store page's embed -- drawing the
first menu item straight through the subtitle. The header now lays out in the
room the buttons leave.

**The map list had never been read from the file that describes it.** It was
looked for one directory up, under field names the generator does not write, so
every launch on every platform fell through to opening all six archives to find
out what they were.

## sdk 1.1

Gearbox 1.1. Nine new capability modules and 94 new imports, taking the ABI to
22 modules and 147 functions. Nothing in 1.0 changed.

## What is new

1.0 was enough to write an overlay and nowhere near enough to write a total
conversion. There were no ships, no armies, no research, no politics, no
economy, no way to author a scenario, and a UI that could draw rectangles and
14pt text.

| Module | What it grants |
|---|---|
| `Military.Read` | Ships -- type, position, health, crew, range -- army stacks per province, fortification and port levels |
| `Military.Write` | Army moves and ship move / engage / bombard orders |
| `Research.Read` | The technology tree, per-country completion, funding |
| `Research.Write` | Set research funding |
| `Politics.Read` | Political compass, policies, province unrest, minorities |
| `Politics.Write` | Enact and cancel policies |
| `Economy.Read` | Gross and net income, army and navy upkeep, bankruptcy, industry level and specialisation, province resources |
| `Economy.Write` | Set province industry level |
| `MapEditor` | Read and write the open map editor project |

Expanded: **UI** gains lines, circles, sized text, `measure_text`, panel
geometry, **your own images**, and the accent colour. **Map** gains coastline,
sea routes and a land test. **Neural** gains the AI's decision space by name.
**Net** gains peer enumeration.

## The two rules

**Every read is bounds-checked** and returns a neutral value -- 0, or an empty
string -- for an id that does not exist, rather than trapping. A mod iterating a
count that changed under it cannot crash the game.

**Every write goes through the same resolver the player's own click goes
through.** Nothing reaches into a container directly. An army order is still
checked for adjacency, a ship order is still clamped to range and routed around
land, a policy is still paid for and still subject to its prerequisites and the
per-turn cap. Granting `Military.Write` lets a mod issue orders, not fabricate
outcomes.

## Reskinning

Three levers, cheapest first:

- `ui/set_theme_accent` restyles the whole interface in one call -- the accent
  is read at over a hundred sites. It is not persisted and is dropped as soon as
  no mod is running, so it cannot outlive uninstalling your mod.
- `ui/draw_image` draws artwork from your own `.odmod` and nothing else. With
  lines, circles and sized text you can build an interface that looks nothing
  like this one.
- `MapEditor` reaches the same per-province data the editor's own tools write,
  so a generator can author a scenario in a loop instead of four thousand brush
  clicks.

Still out of reach: replacing textures the game itself draws outside a mod
panel. There is no central texture registry to hook, and adding one is a
renderer change rather than an ABI change.

## Compatibility

**A mod built against 1.0 runs unchanged, and that is tested.**
`sdk/compat/abi-1.0.json` freezes the 1.0 surface and `tools/check_abi_compat.py`
fails the build if any symbol in it is removed, re-signed, or moved to a
different capability. Within a major version the ABI may only be added to.

The existing conformance test could not have caught that on its own: it checks
that `abi.json` describes the host, and both files move together, so deleting a
function from both passed. All 13 shipped example mods declare
`"gearbox": "1.0"` and still load.

## Fixed

**`gearbox_is_multiplayer` and `gearbox_is_server` always lied.** Both read a
field nothing ever assigned, so they answered "single player, and you are the
authority" in every session. If you wrote a mod that checked before mutating --
the correct thing to do -- you got the wrong answer every time. The same dead
field gated the manifest's `"side"`, so a `"side": "server"` mod was never
masked off on a client.

There is deliberately no `net/is_multiplayer` import: those two already answer
it, and a second way to ask one question is worse than none.

**A mod's accent colour could outlive it.** It was written into the saved
config, so it survived uninstalling the mod with no way back but the reset
button. It now goes to a field the config file does not store.

**The docs and the manifest warning were wrong** about what happens when a mod
imports something the host does not have. It does not "trap on first call" --
an unresolved import cannot be linked, so there is no instance and no first
call. The failure is at load and it names the symbol.

## game 1.0.6a

The game runs on Android, mods can reach most of the game, and a long list of
things that never worked now do.

## Android

Open Doctrines runs on a phone. It is a real port -- a native library packaged
as an APK, not the web build in a wrapper -- and the interface has been resized
for a screen held at arm's length. Touch drives the game, a long press gives the
orders that need a right click, and a settings button sits on the map because a
phone has no ESC key.

Experimental, and labelled that way. It has been verified on an emulator rather
than on a shelf of real devices.

## Things that never worked

**Founding a port, or building your first factory.** The upgrade was looked up
in a province's existing industry or port entry -- which is exactly the entry a
FIRST factory or a NEW port does not have yet. The money was charged, the turns
were waited out, and the build was discarded. Founding a port was a total no-op.
This is also why the AI never industrialised: building was a pure loss, so it
learned to decline it.

**Ports on 127 coastlines.** A province was judged land-locked from the first
patch of water the scan happened to reach. Touch a lagoon and the open sea along
your other edge counted for nothing. On the 1939 map that is 22 British
provinces, 21 American, 17 Soviet, 9 French, and on down -- every one a coast you
can see and the game refused a port on.

**Left-wing doctrines, if you were left wing.** The political compass loaded
with both axes inverted, so the game had you on the opposite side of the board
from where your country actually stood. The Soviet Union loaded as hard right
with none of the four left doctrines available; Germany loaded as hard left with
all of them.

**Doctrine drift surviving a save.** Moving your country's politics is the whole
point of enacting a doctrine, and none of that movement was written to the save.
Governments snapped back to their 1939 positions on load while keeping the
doctrines they had passed to get away from them -- so a player who had worked
their way left found the left doctrines locked again.

**Being told why a doctrine is unavailable.** Every greyed-out doctrine blamed
conflicts you had never enacted. The real reason -- your treasury, or your
compass -- was never shown. Blocked doctrines now say which it is.

**A third of the ships in the game dealt no damage.** Cruisers and battleships
had no entry in the combat damage table and no fallback. The same omission left
them with no sprite. Battleships work now; cruisers are retired, since nothing
could build either.

**Ships sailing through land.** Any crossing whose straight line clipped a
coastline beached the hull. Scenario files also ship about a third of their
boats already aground -- 104 of 340 across the maps -- and those are refloated on
load.

**Ship range.** It bound your mouse and nothing else. An AI boat covered twice
the distance yours could, and no AI hull was slowed by its own speed rating.

**Putting down a rebellion froze your diplomacy.** A revolt is stored as a war,
and both war limits counted it, so a country suppressing a single uprising could
not declare a war or answer a call to arms.

**The terms of use link.** It returned a 404 on every platform for a week. The
terms were written and deployed -- to a service last updated the day before they
existed.

**Signing in from the web, or from a fresh install.** The account service was
only filled in when a config file was read, and neither of those has one, so the
Account screen said no service was configured and told you to edit a file you do
not have.

**Quitting in a browser.** The X and Escape froze the canvas with no way back
but a reload. Neither is offered there now.

## Mods

The Gearbox SDK roughly doubled: mods can now read and command ships and
armies, read and write research, politics and economy, author map projects
directly, draw their own artwork and restyle the interface. Every write goes
through the same rules your own clicks do, so a mod can issue an order but not
invent an outcome.

Existing mods keep working. That is now tested rather than intended -- the 1.0
interface is frozen in the repository and the build fails if any part of it is
removed or changed.

Two permissions -- Audio and Net -- were grantable by a mod and invisible in the
permissions screen, so you could neither see nor revoke them. All of them are
listed now.

## Multiplayer

**Long-form games.** A mode for playing a campaign with nobody online at the
same time: each turn is published, players submit orders back whenever they next
open the game, and a session survives everyone being away for days. Built and
tested, but not yet played through a real multi-day campaign, so treat it as new.

## The AI

Substantially rebuilt -- one shared encoder instead of eight, a longer planning
horizon, a critic that has an opinion about the moves it did not make, and
opponents drawn from its own past selves. Several parts of the network turned
out never to have been training at all.

It also now uses its navy like a player: it routes around land instead of
sailing into it, and it can attack enemy ships, which it previously could not do
at all.

Honestly reported: the model shipping here beats the previous one head to head,
but much of that is the engine fixes above rather than the learning. There is
more to do.

## Elsewhere

Controller support reaches the map, and tells you what each button does. Touch
works in the browser as well as on Android. The update badge is drawn instead of
typed. Windows continuous integration stopped failing on every commit.

## game 1.0.5a

Mostly repairs, and several of them are things that never worked at all.

**If you are on 1.0.4a you must install this one by hand.** The in-game updater
in 1.0.4a asks GitHub a question that can never return an answer, so it will
never offer you this release. That is fixed here; from 1.0.5a onwards the game
can update itself again.

## Things that never worked

**Signing in.** Every shipped copy said "No account service is configured" and
told you to edit a file that is not in the download. It could not be followed by
anyone who did not build the game themselves. The account service is now part of
the build.

**Updating.** The updater asked GitHub for the "latest release", an endpoint that
skips pre-releases by design -- and every alpha is one. It answered "nothing
found" for the entire life of the game, so no copy has ever been offered an
update. It now asks a question that has an answer.

**Playing in the browser on an ordinary machine.** The web build demanded 2 GB of
memory before it drew anything, and a scenario needs a good deal more on top of
that. Machines that could not spare it got a working menu and scenarios that
would not load. It now starts at 512 MB and grows only as needed.

**Exporting a timelapse GIF on Windows**, and **opening a multiplayer tunnel on
Windows**. Both ran commands that only exist on macOS and Linux.

**Playing at all, if your Windows account name is not plain English.** Every file
the game opened went through a text encoding that cannot represent most names, so
a Cyrillic or Japanese account name meant every single file failed to open. The
game started into a world with no fonts, no maps and no scenarios, and said
nothing about why.

## Things that now explain themselves

Several failures used to end in silence, which is the worst way to meet one.

- A scenario that fails to load now says which file and what tends to cause it,
  instead of returning you to the menu with no message.
- An empty scenario list now says the data folder is missing, and names where it
  looked. The most common cause is running the game from inside the .zip; the
  download now carries a READ ME FIRST explaining it.
- A graphics driver too old for the game now says so in a dialog rather than the
  game appearing not to start.
- Music no longer loops a fragment while the browser asks whether you meant to
  leave the page.

## Under the hood

The Windows build is now actually run by the build server rather than only
compiled there, including a full packaged copy playing a real scenario, so the
class of fault above cannot reach a release unseen again.

## Before you install

Both the zip and the installer are **unsigned**. Windows SmartScreen and macOS
Gatekeeper will warn on first launch; the README explains how to get past each.

There is no Linux Arm build. GitHub hosts no Arm Linux runner, so that platform
builds from source.

## game 1.0.4a

The first public release of the OpenDoctrines alpha.

Six historical scenarios on a 1641-province world map, a map editor for building
your own, multiplayer that needs no port forwarding, and a mod SDK for thirteen
languages. Playable in the browser as well as on Windows, macOS and Linux.

Alpha means it is playable from end to end and is not finished. The README's
Status section says which platforms have actually been sat down in front of.

## The AI fights again

The learned AI had gone quiet -- declaring no wars at all, 0.00 per thousand
country-turns against 4.72 for a control that picks at random, and playing the
map as though the only safe move were no move. Four separate causes, each enough
on its own:

- Wars were charged twice, once as aggression and once as a phoney-war penalty,
  so a war that went well still cost more than never declaring one.
- The idle penalty was combined across every module, so the war module could sit
  out an entire game uncharged as long as the economy was busy.
- The action mask offered wars the executor then refused to declare, so the
  policy was rewarded for choosing something that never happened.
- Rebel countries were counted in the trained AI's own statistics, making its
  behaviour unreadable exactly when it needed reading.

It now takes 32.4% of the land it plays for, up from 13.4%, and declares 1.73
wars per thousand country-turns. It still loses games it should win. That is
what the alpha label is for, and the game says so in as many words.

## Rebellions settle down

A province that put down a revolt could revolt again the next turn, because the
roll had no memory of the one before it. Across a 250-turn game that compounded
into 945 revolts and 921 rebellion wars, with one province rising 25 times.
Provinces now hold a cooldown once a rebellion is resolved: about 450 revolts
over the same game, and no province rising more than four times.

## Windows starts

The Windows build did not run. Double-clicking it did nothing at all -- no
window, no error, no crash dialog -- on any machine without the Visual C++
redistributable installed, because the executable depended on DLLs that are not
part of Windows and the package shipped none. The C++ runtime is now linked into
the executable, so there is nothing to install first.

And when the graphics driver genuinely cannot run the game, it now says so in a
dialog naming the cause, instead of vanishing silently. OpenGL 3.3 is required;
a machine without it gets an explanation rather than nothing.

## Play it in the browser

The web build ships as a release of its own, so the game can be tried without
downloading anything.

## Smaller downloads

The installers carried the AI trainer's entire workspace -- per-worker
checkpoints, backups, superseded models -- because the shipped data was chosen a
directory at a time and that directory doubles as scratch space. Installed size
drops from about 215 MB to 102 MB, with no change to what the game reads. The
browser build had the same problem in a worse place, and is down from 118 MB to
75 MB.

## Before you install

Both the zip and the installer are **unsigned**. Windows SmartScreen and macOS
Gatekeeper will warn on first launch; the README explains how to get past each.
Code signing needs a certificate this project does not have yet.

There is no Linux Arm build. GitHub hosts no Arm Linux runner, so that platform
builds from source.

## game 1.0.3a

The first tagged release of the OpenDoctrines alpha.

Six historical scenarios on a 1641-province world map, a map editor, multiplayer
that needs no port forwarding, and a mod SDK for thirteen languages. Alpha means
the game is playable end to end and is not finished; the README's Status section
says which platforms have actually been sat down in front of.

## The AI fights again

The learned AI had gone quiet. It was declaring no wars at all -- 0.00 per
thousand country-turns, against 4.72 for a control that picks its decisions at
random -- and playing the map as if the only safe move were no move. Four
separate causes, each of which alone was enough:

- Wars were charged twice, once as aggression and once as a phoney-war penalty,
  so a war that went well still cost more than never declaring one.
- The idle penalty was ANDed across every module, so the war module could sit
  out a whole game without ever being charged for it, as long as the economy
  was busy.
- The mask offered wars the executor then refused to declare, so the policy was
  rewarded for choosing an action that never happened.
- Rebel countries were counted in the trained AI's own statistics, which made
  its behaviour unreadable at exactly the moment it needed reading.

Against the random control the AI now takes 32.4% of the land it plays for, up
from 13.4%, and declares 1.73 wars per thousand country-turns. It is still
learning and still loses games it should win. That is what the alpha label is
for, and the in-game text says so.

## Rebellions settle down

A province that put down a revolt could revolt again the next turn, because the
roll had no memory of the one before it. Over a 250-turn game this compounded
into 945 revolts and 921 rebellion wars, with a single province rising 25 times.
Provinces now hold a cooldown after a rebellion is resolved: roughly 450 revolts
across the same game, and no province rising more than four times.

## Packaging

The installers carried the AI trainer's entire workspace -- per-worker
checkpoints, backups and superseded models -- because data/ was allowlisted a
directory at a time and that one directory is also scratch space. Installed size
drops from about 215 MB to 102 MB, with no change to what the game reads.

## Before you install

Both the zip and the installer are **unsigned**. Windows SmartScreen and macOS
Gatekeeper will warn on first launch; the README explains how to get past each.
Code signing needs a certificate this project does not have yet.

There is no Linux Arm build. GitHub hosts no Arm Linux runner, so that platform
builds from source.

