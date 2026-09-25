# Testing multiplayer on one machine

## One command

```bash
tools/playtest.sh              # a playing host and three joiners
tools/playtest.sh --spectate   # host takes no seat; three joiners play
tools/playtest.sh --verify     # no windows: check it, and exit non-zero if wrong
tools/playtest.sh --clean      # throw the sandbox away
```

Four clients, four different players, no second Google account. It runs the
local dev issuer (`tests/mock_issuer.mjs`), gives each client its own data
directory, and writes each one a session directly -- Alice, Bob, Carol and
Dave. Plain http is allowed for exactly this case and no other:
`AccountClient` permits an insecure issuer only on `localhost`.

Identity comes from the token, so `dev-alice` is **always** Alice. That
stability is the point -- a pseudonym that changed per request would make every
rejoin look like a stranger and seat memory could never be tested.

Ctrl-C closes all four and the issuer. Logs land in `.playtest/*.log`.

### Why four and not two

Two peers cannot fail in the ways that matter. Seats that collide, a country
claimed twice, a turn that begins for whoever was pumped last, a reconnect that
hands somebody another player's country -- every one of those is *fine* with one
joiner and wrong with three.

`--verify` is exactly those cases with nobody at the keyboard: it brings up a
real host and three real clients over loopback against the dev issuer, seats all
four, resolves a turn, disconnects one player, resolves another turn without
them, and reconnects them to check they get their **own** country back. It exits
non-zero if any of that is wrong, which is what makes it the thing to run when
qualifying a platform. See `testParty()` in `tests/net_connect_test.cpp`; it also
runs as part of `tests/run_all.sh`.

What `--verify` cannot judge is how any of it *looks*. That is what the windowed
modes are for.

**What it does not cover:** the account screen itself. These clients arrive
already signed in, so OAuth, account creation and the consent gate are not
exercised -- everything after sign-in is.

---


The awkward part is not running two copies. It is that **a psid is derived from
the account you sign in with**, so two copies signed in as you are one player
twice — and the lobby is right to say so. `admit()` sees a psid it already
knows, takes the reconnect path, and hands the second copy the first one's seat.
That looks like a lobby bug. It is not.

So there are two separate problems, and they need separate answers.

## 1. Two copies must not share a data directory

One `data/` means one `account.json`. The second sign-in evicts the first, both
copies end up presenting the same session, and their saves and server lists
overwrite each other.

```bash
tools/second_player.sh
```

That builds `data-p2/` with its own `account.json`, `config.json`,
`servers.json` and `saves/`, and **symlinks** the bulk assets — maps, flags,
audio — because neither instance writes to them and copying a map pack to prove
a point is silly. Then:

```bash
OD_DATA_DIR=./data-p2 ./cmake-build-debug/OpenDoctrines.app/Contents/MacOS/OpenDoctrines
```

`OD_DATA_DIR` works for any directory; the script just makes a sensible one.

## 2. Two players need two identities

This one has no trick. A pairwise pseudonym is a function of the account, so:

| What you have | What you get |
|---|---|
| One account, two copies | One player, twice. The join reads as your reconnect. |
| One account, "Host only" ticked | A real join — but still one player at the table |
| **Two provider accounts** | **Two players. This is the real test.** |

A second GitHub or Discord login is the usual way; the game accepts any of the
three providers, so your Google account hosting and a GitHub account joining is
two people as far as everything downstream is concerned.

### Testing without a second account

You can still exercise most of it:

- **Host only — I am not playing** (Rules tab) means the host takes no seat, so
  your one account can join its own server as the only player and you can drive
  a full turn: claim a country, press Ready, watch it resolve.
- **Seats and resuming** are covered by `NetSeatBookTest` without any network at
  all — reserve, reconnect, release, and the max-players interaction.
- **Joining, refusal and slow links** are covered by `tests/connectivity_test.sh`
  against `tests/mock_issuer.mjs`, which mints a fresh pseudonym per ticket. That
  is the closest thing to several players that exists without several accounts,
  and it is what the automated suite uses.

The mock issuer cannot stand in for interactive sign-in — it has `/session` and
`/ticket` but no OAuth — so it tests the join path, not the account screen.

## Silence, which is what players mostly do

`--verify` keeps both ends talking in a tight loop, and for a long time that was
the whole of the automated coverage. Players do the opposite: they sit in a
lobby without clicking, and their game stops pumping its socket entirely while
it loads the world the host just sent. All of that read as a dead connection,
and the first people to join a 1.2.2a game were cut off mid-lobby and mid-start.

So `tests/connectivity_test.sh` has two more cases, and they are the ones to run
when anything near the transport changes:

- **`quiet`** — a lobby nobody touches, a client whose game thread is busy, a
  host whose own frame loop is blocked, and a four-megabyte world. The
  intervals are shortened through `OD_NET_DEAD_SECONDS` and `OD_WS_PING_MS` so a
  60-second rule can be watched in four; nothing in the game sets either.
- **the same case again with `OD_WS_NO_KEEPALIVE=1`**, which makes the client
  behave like every copy of 1.2.2a in the wild: it says nothing at all, and what
  has to carry it is the host's ping and the reply the WebSocket layer has
  always sent. Mixed versions are the normal case for a while after a release,
  and this is the arm that proves they still work.

Each of the four fixes behind those cases was put back in turn to check the
case fails without it.

```bash
tests/net_race_test.sh        # ThreadSanitizer over the lobby, needs node
OD_TSAN=1 tests/run_all.sh    # the same, from the full suite
```

`net_race_test.sh` builds the connectivity test with ThreadSanitizer and reads
the lobby while a host opens -- the pattern behind "the server crashes when I
press the invite button". It runs in CI as its own job; it is opt-in locally
because it wants a build of its own. The stand-in issuer takes `--delay-host`
for it: a real account service is slow for a HOST too, and without that delay
the window the race lives in never opens.

## What CI runs, and what it cannot

Every platform job runs the whole suite through `tools/qualify.sh`, and two
things were added after each of them let a real bug through:

- **A real HTTPS request** (`tests/net_live_check.cpp`), on all four desktop
  platforms. The suite's only networking is plaintext to localhost, so the TLS
  path -- and each platform's own certificate loading inside it -- was never
  entered by anything automated. Windows shipped a release that loaded no roots
  at all and could not sign in, with CI green throughout. The check fails only
  when the machine cannot VERIFY a server it reached; "nothing answered" is
  treated as the runner's network, because a check that cries wolf is one
  nobody keeps.
- **Android tests on an emulator** (`android-run`). Nothing built for Android
  was ever executed: the platform's job compiled a library, checked it was a
  NativeActivity, and packaged it. Android-only behaviour was therefore only
  discovered by somebody holding a phone -- a swipe that could not pan the map,
  a keyboard that did not exist, a transport that had not compiled since it was
  written. The job runs the parts whose answers are the same everywhere and
  were simply never asked there.

Neither can judge how anything feels. An emulator does not have a thumb, and
the live check says nothing about whether sign-in is usable once it connects.

## What is still not covered by anything

Two live clients taking a turn against each other. Specifically: one player
timing out while another is still thinking, and the per-player clocks that
follow from it. The turn loop needs a real host to exercise, so it has no
automated coverage. If something is going to be wrong, that is where.

Nor is a real tunnel. The keepalives above are what cloudflared's idle timeout
needs, and they are tested against a host that keeps its sockets open rather
than against cloudflared itself -- which would mean a live tunnel in CI, and a
test that fails when somebody else's free service is busy.
