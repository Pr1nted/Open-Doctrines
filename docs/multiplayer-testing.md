# Testing multiplayer on one machine

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

## What is still not covered by anything

Two live clients taking a turn against each other. Specifically: one player
timing out while another is still thinking, and the per-player clocks that
follow from it. The turn loop needs a real host to exercise, so it has no
automated coverage. If something is going to be wrong, that is where.
