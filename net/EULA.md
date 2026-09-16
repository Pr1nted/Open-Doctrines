# Installing a mod: what you are agreeing to

**This is a draft and has not been reviewed by a lawyer.** It is written to be
read rather than skipped, which is not the same as being legally sufficient.

This covers **mods you find through the OpenDoctrines mod directory**. The game
itself is free software under the OpenDoctrines Non-Commercial License; nothing
here changes that, and nothing here is a licence for the game.

## The short version

A mod is somebody else's program. We did not write it, we have not run it, and
you are choosing to run it. The game puts real limits on what it can do, and
those limits are the protection — not us, and not the check on its listing.

## Who you are getting it from

**The author, not us.** The directory holds a description and a link; the file
comes from a server the author chose. When you download a mod you are dealing
with them.

That means:

- **The mod is under whatever licence its author gives it.** Look at their page.
  We do not impose one and we take no rights in it.
- **Any promise about it is theirs.** We make none.
- **If it is not what it claimed to be, that is between you and the author.** We
  will take a listing down when it is reported and wrong — that is all we can
  do, and it does not undo a download.

## What the check on a listing means

Every listing is checked before it appears: the hash the author declared is
looked up with a third-party scanner, and the download link is tested.

**That is not a review, a test, or approval.**

- `clean` means no engine has reported those particular bytes. It is not a
  statement that the mod is safe.
- `unknown` means nobody has ever scanned that file. It is the ordinary state of
  a new mod, and it is not a warning.
- The hash was **declared by the author**. We never fetch the file, so we cannot
  promise that what is at the link today is what was scanned.

Nobody has run these mods. If that matters to you, read the source — most mod
authors publish it, and the listing links to their page.

## What the game does to keep a mod in its place

This is the part that actually protects you, and it is worth knowing:

- A mod runs in a **WebAssembly sandbox with no ambient authority**. It cannot
  open, read, delete or list a file: those capabilities are not linked into it,
  so there is nothing to bypass.
- It gets only the **capabilities you granted**, listed on its page before you
  install and revocable afterwards.
- It runs under **memory and execution limits**, so a mod cannot exhaust your
  machine or hang a turn indefinitely.
- **The game never downloads or installs a mod by itself.** You fetch it, you
  add it. That is deliberate, and a directory is not a reason to change it.

One honest gap: on the **web build**, the execution limit is not enforced,
because browsers expose no equivalent. A mod with an infinite loop in a hook can
hang that tab. Memory limits and capability limits work everywhere.

## Multiplayer

A mod marked **synchronised** must be the same file, at the same version, for
everyone in the game. A joiner whose mods do not match the host is refused.

This is an **integrity check, not an anti-cheat**. It catches the wrong version,
a truncated download, a mod somebody edited and forgot to rebuild. It cannot
stop somebody lying about what they are running: a client is a program on
hardware its owner controls. Cheating is prevented by the server being
authoritative, not by this check.

## Mods and your saves

A mod can change how a game plays. A save made with a mod may not load the same
way without it. That is not a fault, and we cannot recover a game you played
with a mod you later removed.

## No warranty, from us

The directory is provided as it is. We do not promise it will be available,
correct, or that any listing is accurate. **We are not liable for anything a mod
does to your game, your saves, or your computer.**

> **[FOR LEGAL REVIEW]** A limitation of liability needs to be drafted against
> the jurisdictions the project actually reaches — the developer is in the EU
> and the audience includes schools, where consumer-protection rules may make a
> broad exclusion unenforceable. This paragraph states the intent; it is not
> yet the clause.

## If you are under 18

Ask whoever is responsible for the computer before installing a mod. This game
is played in classrooms, and a mod is third-party software however friendly its
page looks.

## Changes

If this changes in a way that affects what you are agreeing to, publishing a mod
will ask you again. For downloading, the version in force is the one on this
page.
