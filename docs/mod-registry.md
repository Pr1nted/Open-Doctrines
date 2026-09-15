# The mod registry

A directory where mod authors advertise their mods, on the account service
(`net/`). It lists mods. It does not hold them.

## Why it holds nothing

Two reasons, and the second is the one that decided it.

**Cloudflare's free plan.** The service runs on it, and mod files are the sort
of thing that would end that.

**The game already refuses to fetch a mod, on purpose.** `src/mods/ModUpdates.h`
puts it as *"LOOKS, NEVER TOUCHES"*, and `Game_Mods.cpp` says it again where the
button is drawn: the game never downloads or installs a mod, "because that would
mean fetching and running code chosen by a third party, which is the one thing
the whole capability sandbox exists to prevent." A registry that held the files
would be the natural place to quietly undo that decision. So it does not hold
them, and a player still fetches a mod themselves and adds it themselves.

What a listing is for, then, is being *found*.

## What a listing is

A JSON record under `pkg:mod:<id>` in the one KV namespace. The id is the
`MANIFEST.json` id, because that is what the game pins trust to, and a listing
under any other id is a listing nobody can install.

| Field | Note |
|---|---|
| `id`, `name`, `version`, `summary`, `description` | `id` is immutable and is the identity |
| `page` | Where a person reads **about** the mod. What browse links to |
| `downloadUrl` | Where the `.odmod` is. Reached through `/mods/<id>/get` |
| `sha256` | **Declared** by the author. See below |
| `modules` | The capabilities `MANIFEST.json` asks for. Shown, because a mod asking for `GameProcess` changes how turns resolve and one asking for `UI` cannot |
| `side`, `gearbox`, `tags`, `thumbnail` | Mirror the manifest |
| `scan` | What VirusTotal said about the declared hash, if anything |

URLs are held to the same rule the game applies before it will fetch an
`updateUrl` — https, bounded length, no character that could end a shell word.
Not because a Worker has a shell, but because a URL this service accepts and the
game later refuses is a listing that looks published and cannot work.

## The key prefix is `pkg:`, not `mod:`

`mod:` was already taken by **moderation** — `mod:report:` and `mod:psid:`. Two
features called "mod" meaning different things share a namespace exactly once.

## Counting downloads

`GET /mods/<id>/get` counts, then 302s to the author's host. The redirect is the
only reason the count can exist at all: a link straight to the author's server is
a download nobody here can see.

The counter is a **Durable Object, one per mod**, not KV. This is not a
preference. The free plan allows **1,000 KV writes a day**, shared with
everything — creating an account spends three, which is what puts the ceiling at
a few hundred signups a day. A download counter in KV would burn that budget on a
few hundred clicks and then start failing account creation. DO requests bill
against a separate 100,000/day budget and SQLite can do the read-modify-write
atomically, which KV cannot do at all.

### "Unique" means per-day unique, and the number says so

Counting unique downloaders for all time means keeping a per-person marker for
all time, and a keyed hash of an IP held indefinitely is still a record of who
visited. `PRIVACY.md` promises no address is kept.

So the marker is **salted with the date** and dropped after two days. What is
stored is `HMAC(IDENT_KEY, "dl:<day>:<mod>:<ip>")` — never the address — and it
answers "how many different people today" without outliving the question.

The cost, which must be stated wherever the number is: **somebody who downloads
on Monday and again on Friday counts twice.** The API returns `uniqueBasis`
alongside the figure so a client cannot render it as "unique people".

## Review: the queue is shaped by the scanner's rate limit

A new listing is **`pending`** and does not appear in the directory. A cron
trigger drains a queue, checks it, and only then does it become `listed` — or
`held`, if the check found something.

**This is not a workflow preference. It is arithmetic.** The first version
looked the hash up inside the publish request, and VirusTotal's free tier allows
**four lookups a minute and 500 a day**. Two authors publishing in the same
minute meant the second got a 429 — and a failed lookup is correctly recorded as
*no* lookup, so that listing went up reading "unscanned" and nothing ever came
back to it. The check quietly stopped happening exactly when the directory got
busy enough to need it.

So the rate limit shapes the process instead of breaking it:

| | |
|---|---|
| Cron | `* * * * *` — once a minute |
| Per drain | 4, because that is the tier's per-minute allowance |
| Per day | 480, leaving 20 of the 500 as headroom |
| Budget counted | at **lease** time, not on success — a lookup that 429s still spent a request |
| Retry | the lease lapses after 5 minutes and a later tick picks it up |
| Give up | after 3 tries it is **listed anyway**, reading "unscanned" |

That last row is deliberate. Three failed lookups is evidence that VirusTotal is
unreachable, not that the mod is bad, and holding somebody's work hostage to our
own outage would be the wrong way round.

The queue is one Durable Object, because a queue's job is to be a single line
and the daily budget has to be counted exactly once across concurrent drains —
a read-modify-write KV cannot do atomically. **Order is a sequence, not a
clock**: ordering by a second-resolution timestamp ties whenever two people
publish in the same second, and then "first come, first served" is whatever
order SQLite happens to return.

### What sends a listing back for review

Only a **changed file** — a new listing, or one whose declared SHA-256 moved.
Editing a summary does not, and that matters more than it looks: the budget is
500 lookups a day for the whole service, so re-reviewing on every edit would let
one author fixing typos spend the day's checks and push everybody else's first
review to tomorrow.

Replacing the file also clears any existing hold, because leaving it would
punish the author for having fixed the problem.

### What the author sees

The publish response says `status: "pending"` with a queue position and a rough
wait, and `/mods/mine` carries the position and the hold reason. A hold that
does not say what is wrong is a listing nobody can fix.

## The security check, and what it is not

If `VIRUSTOTAL_API_KEY` is set, the review queue looks the declared SHA-256 up
against VirusTotal's corpus. Four outcomes, and they stay four:

| State | Means |
|---|---|
| `flagged` | Engines have seen this hash and some call it malicious — the listing is **held** |
| `clean` | Engines have seen this hash and none do |
| `unknown` | **VirusTotal has never seen this hash.** Listed anyway — see below |
| `unscanned` | We could not ask — no key, or we gave up after three tries |

`unknown` does **not** hold a listing. Never having been uploaded to VirusTotal
is the normal state of a new mod, so holding for it would hold every mod ever
published. What holds a listing is an engine actually reporting the file, or a
download link that answers with an HTTP error.

It is *also* the state of a file written this morning to attack somebody.
Rendering it as clean would turn a weak signal into a false assurance, so it is
its own word and carries its own sentence in the payload.

Two further limits, both real:

- **The hash is declared, not observed.** We never fetch the `.odmod`, so we
  cannot say the bytes at the download URL still hash to what we looked up. An
  author could publish one file, get it scanned, and serve another.
- **A clean result is not an endorsement.** It means nobody has reported those
  particular bytes.

A lookup that fails is never recorded. An absent scan renders as `unscanned`,
which is true; writing a verdict because the lookup failed is the one thing this
must not do.

## Moderation is scoped, and that is the feature

Mod reports have their own queue (`pkg:report:`), their own reasons
(`malware`, `stolen`, `illegal`, `sexual`, `broken`, `spam`, `other`) and their
own outcomes: `unlist`, `restrict`, `dismiss`.

There is no `ban` here, deliberately. Banning an account is
`moderation/reports.ts`'s power, it is decided on the evidence that file
collects, and a complaint about a mod is not that evidence.

`restrict` writes `account.restricted.mods`, a field **nothing on the join path
reads**. Someone who published malware stops being able to publish; they do not
stop being able to play a game they have done nothing else wrong in. Collapsing
the two would leave a moderator holding one lever whose only setting is "remove
this person from the game" — and a moderator with that lever either uses it or
does nothing.

## Tags, and why `modmaker` is not a badge

`accounts/badges.ts` is explicit: badges are granted by hand, "no self-serve path
and no automation, because the whole value of a badge is that it means somebody
decided."

An automatically earned tag therefore cannot be a badge without contradicting
that. `modmaker` — five listings or more — is **computed on read** from the
listings the account holds. It costs no write, cannot drift, appears at the
fifth and goes away again if listings are withdrawn.

`POST /account/tag` chooses which held tag is displayed. It is validated when set
*and* again when rendered, because a tag can be earned and then lost, and a
stored display choice must not outlive the thing it displays.

## Routes

| Route | Who | What |
|---|---|---|
| `GET /mods` | anyone | One page, newest first, `?cursor=` |
| `GET /mods/<id>` | anyone | One listing, with live counts |
| `GET /mods/<id>/get` | anyone | Count, then 302 to the author's host |
| `GET /mods/guidelines` | anyone | The guidelines and their version |
| `POST /mods/guidelines` | account | Agree to the current version |
| `POST /mods` | account | Publish or update |
| `GET /mods/mine` | account | Own listings, including unlisted ones |
| `POST /mods/<id>/withdraw` | owner | Remove it, and its counts |
| `POST /mods/report` | account | Report a listing |
| `GET /moderation/mods` | developer | The queue |
| `POST /moderation/mods` | developer | `unlist` / `restrict` / `dismiss` |

`GET /moderation/mods` also returns the queue's depth and remaining daily
budget, because a drain that has stalled looks exactly like a directory nobody
is publishing to.
| `POST /account/tag` | account | Choose the displayed tag |

Browsing and downloading need no account. Requiring one would make the directory
useless to most of the people it exists to reach.

Newest-first ordering is built into an index key (`pkg:new:<ceiling - created>`)
because KV lists lexicographically and offers no other order. It costs one extra
write per publish, and publishing is rare. **There is no ranking by downloads** —
that would need an index that is rewritten every time somebody clicks, which is
the KV write budget again.
