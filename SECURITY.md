# Security policy

## Reporting

**Use GitHub's private vulnerability reporting:**
<https://github.com/Pr1nted/Open-Doctrines/security/advisories/new>

That is the only private channel. No email address is published, because the
developer works under a pseudonym and an address would undo that — this is not
an oversight and there is no better address to ask for.

**Do not open a public issue for a vulnerability.** Report it rather than
publish it first; that sentence is also in
[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md), and this is the mechanism behind it.

Useful in a report, roughly in order of how much it helps:

- what an attacker gets, stated as a capability — reads another player's
  orders, escapes the mod sandbox, executes code on a machine that opened a
  map, impersonates an account
- the version and platform: `v1.0.8a`, `server-v1.0.8a`, a commit hash, or the
  build you took from a release page
- steps, or a file, or a few lines of code that reproduce it
- what you think the fix is, if you have a view

A proof of concept is welcome. A file that reproduces a crash is worth more
than a description of one.

### What to expect

One person maintains this, in their own time, so what is promised here is
effort rather than a schedule: an acknowledgement within a week, and an honest
answer about whether and when it will be fixed rather than silence. If a report
turns out to be something already known and written down — the list under
"Known limits" is the usual case — you will be told that, with a pointer to
where it is recorded.

Fixes ship in the next release of whichever track is affected. If something is
severe enough to warrant a release on its own, it gets one.

You will be credited in the advisory and the release notes unless you would
rather not be, and pseudonyms are fine here for the same reason they are fine
for the developer. Please hold off on publishing until a fix is out, or until
it is clear one is not coming.


## Supported versions

Three things release on their own tags, and only the newest of each gets
fixes. Nothing is backported.

| Track | Current | What it is |
|---|---|---|
| Game | `v1.0.8a` | The client, on desktop and web; the Android build is experimental |
| Dedicated server | `server-v1.0.8a` | The headless host |
| Gearbox SDK | `gearbox-v1.1` | The mod ABI and its language SDKs |

Every one of them is alpha. Treat "supported" as "will be fixed if reported",
not as a maintenance guarantee.

The account and relay service in [`net/`](net/) has no version. It is
whatever is deployed, and a fix there ships when it is written.


## In scope

- **The account and relay service** (`net/`). It is live, it holds account
  records, and it issues the tokens everything else trusts. Highest value
  target here by a distance — token confusion between the two audiences,
  ticket replay, pseudonym linkage across servers, or anything that lets one
  session reach another.
- **The multiplayer stack** (`src/net/`). Forging or reading another player's
  orders, taking over a session, getting invalid orders past the host's
  validation, or crashing a host with a malformed packet.
- **The Gearbox mod sandbox** (`src/mods/`, `docs/gearbox-abi.md`). Escaping
  the WASM sandbox, reaching a capability that was not granted, or getting out
  of linear memory through a host function's bounds check.
- **File parsing.** `.odmap`, `.odsv`, `.odmod`, and the image and archive
  decoders behind them. These read files that arrive from strangers, which
  makes memory corruption here the most likely way to get code execution on a
  player's machine.
- **The updater** (`src/GameUpdates.cpp`). Anything that lets an update come
  from somewhere other than the release hosts, or that gets a path out of the
  archive and onto the disk outside the install directory.
- **The dedicated server**, including its container images.

Vendored third-party code counts if the project's use of it is what makes the
bug reachable. If the bug is upstream, report it upstream too — the components
and versions are listed in [NOTICE.md](NOTICE.md).


## Out of scope

Not because these do not matter, but because they are decisions rather than
defects, and a report about one will be closed with a pointer here.

- **Mods, maps and content other people made.** You decide what to install,
  and the capabilities a mod asks for are shown before you grant them.
  [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) §4 is the longer version.
- **A host you chose to join behaving badly within the rules.** The host is
  the authoritative server; joining a stranger's game is a trust decision the
  game asks you to make knowingly. What the protocol deliberately keeps from
  that host — your account identity, your orders before they resolve — is in
  scope, and is in [net/PRIVACY.md](net/PRIVACY.md).
- **Destroying a long-form turn by overwriting its blob.** Turn stores are
  dumb buckets and are not trusted with availability; see the header comment
  in `src/net/TurnSeal.h`. Forging or reading a turn is a different matter and
  is in scope.
- **Volumetric denial of service** against the free-tier relay, or against a
  host on someone's home connection.
- **Missing compiler hardening flags, headers or best practices** with no
  demonstrated impact, and raw scanner output with no analysis behind it.
- **Social engineering, physical access, and compromise of the developer's
  own accounts.**
- **Cheating in single-player.** It is your machine and your save.


## What the architecture already assumes

Worth reading before hunting, because it says where the real boundaries are
and where there are none.

- **Mods have no ambient authority.** A mod is WebAssembly with no filesystem,
  no network, no processes and no precise clock. Everything it can reach
  arrives as an explicit import through `modHostFunctions()`, gated on a
  capability the user granted; a mod importing anything else does not
  instantiate. Every pointer argument is a linear-memory offset the host
  bounds-checks itself, deliberately rather than through the runtime's pointer
  sugar, so both backends see the same argument list and the check is in code
  that can be tested.
- **The relay never parses a game payload.** It moves bytes and vouches for
  who sent them. The rules live in the C++ game.
- **A game server never learns which account it is talking to.** Join tickets
  carry a per-server pseudonym, `HMAC(key, accountId + ":" + serverId)`, and
  cannot call the account API. Session tokens can, and never leave the
  player's client. That split is load-bearing; treat anything that collapses
  it as a finding.
- **Orders are sealed before they leave the player's machine** and opened only
  by the host, so a turn store cannot read or forge them.

## Known limits, said rather than hidden

- **No sandbox is perfect.** The mod system is designed so that a bug is a bug
  rather than a missing check, but it is software. LICENSE clause 7 is the
  legal version of this sentence.
- **The updater verifies integrity, not provenance.** It refuses any download
  not served from the release hosts, and checks the SHA-256 that the release
  metadata carries — but that digest arrives over the same connection as the
  file, so it catches a truncated or corrupted download, not a compromised
  GitHub account. There is no separate signing key. Saying so is more useful
  than implying the checksum proves something it does not.
- **The builds are not code-signed.** The Windows build is unsigned, which is
  why SmartScreen objects to it, and the Android APK is signed with a debug
  key. Neither is a certificate you can check anything against. Until that
  changes, the provenance of a binary rests on where you downloaded it from.
- **A blob store can destroy a long-form turn**, which is an outage rather
  than a rigged game. The distinction is deliberate and is documented where
  the code lives.
- **Windows builds are verified in CI rather than on real hardware**, so
  platform-specific issues there are likelier to survive review.

<!-- review-gate test: this line is not meant to be merged. -->
