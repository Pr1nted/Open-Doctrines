# Nickname blocklist

The list of blocked terms is **not committed**. It is uploaded to KV at deploy
time:

```bash
npx wrangler kv key put --binding OD_ACCOUNTS cfg:blocklist --path blocklist/profanity.txt
```

Two reasons it lives here rather than in the source:

1. The repository does not need to carry a list of slurs, and neither does
   anyone reading the code.
2. A gap found on a Friday can be closed with one `kv key put` instead of a
   redeploy.

`profanity.txt` is gitignored. Create it from a maintained public list — the
usual starting point is
[LDNOOBW](https://github.com/LDNOOBW/List-of-Dirty-Naughty-Obscene-and-Otherwise-Bad-Words),
which covers many languages — and then prune it, because those lists are built
for filtering prose and are aggressive for names.

## Format

```
# comments start with a hash
badword
another term

!scunthorpe
```

- One term per line.
- A leading `!` marks an **exception**: a nickname allowed in full even though it
  contains a blocked substring.
- Terms are compared against the *normalized* form of a nickname — casefolded,
  separators stripped, leetspeak undone, repeated letters collapsed. So one
  entry covers every spelling: `badword` already blocks `B4D-W0RD`,
  `b_a_d_w_o_r_d` and `baaadword`. **Do not add variants**; they are redundant
  and they make the list harder to audit.

## Exceptions are not optional

Substring matching over a profanity list produces false positives. It always
has: real place names, real surnames, and ordinary words that contain a rude
substring. The classic is Scunthorpe.

Without an escape hatch the only way to unblock a legitimate name is to weaken
the filter for everybody, so the `!` lines exist to keep the trade local. When a
player reports a name that should be allowed, add an exception rather than
removing a term.

## What is NOT in this file

Reserved and impersonation words — `admin`, `moderator`, `staff`, `developer`,
`playtester`, `opendoctrines` and so on — live in `RESERVED` in
`src/accounts/nickname.ts`. They are committed because there is nothing
objectionable about reading them, and because they are part of how badges stay
meaningful: a nickname must not be a claim about who you are.
