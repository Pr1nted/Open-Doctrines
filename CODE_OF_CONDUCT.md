# Code of Conduct

This document says what OpenDoctrines tries to be, what it does not try to be,
and where the developer's responsibility starts and stops. It is written to be
honest about limits rather than to make promises that cannot be kept.

## 1. Safety

There will be bugs. Some of them will be security bugs.

The project tries to be safe **by architecture** rather than by vigilance. A
mod runs as WebAssembly in a sandbox with no ambient authority: no filesystem,
no network, no processes, and no clock precise enough to identify a machine. It
receives exactly the capabilities it declared and you granted, as imports, and
anything it did not ask for does not exist inside it. That is a structural
property, not a promise to remember to check something.

Community-made content can still break one thing or another. When a bug is
known, the aim is to fix it to the best of the developer's ability. That is a
commitment to effort, not a guarantee of outcome — and the licence provides the
software as is, without warranty.

If you find a security issue, report it rather than publish it first.

## 2. Customisability

The project aims to be as customisable as it can reasonably be — mods, maps,
scripts, saves, the lot.

The developer believes that making source open, or at least available, does
others a service. Code you can read is code you can learn from, verify, and fix.
With that openness come suggestions, and suggestions that look necessary may be
implemented.

This is why the mod system is a real ABI with thirteen language SDKs rather
than one scripting hook: customisability that only works in the language the
developer happened to pick is not really customisability.

## 3. No harm intended

No harm is intended with this game.

This project is **not a political statement**. It is a project that aims to be
used for entertainment, and nothing else. Countries, borders, ideologies and
events that appear in it are systems in a game, not positions the developer
holds, endorses, or is arguing for.

If something in the game reads as a statement, that is not the intent.

## 4. Community content is essential, but the developer is not liable

Community content is the point. It is also outside the developer's control.

Some community content will not follow this code of conduct. When that happens
it is not the developer's responsibility to moderate what you choose to
install, watch, or play. You decide what to run, and the capability list a mod
requests — shown to you before you grant it — is there so that decision is an
informed one.

Where there is a community-wide problem that the developer team can actually
influence, the developer reserves the right to do what they believe is
reasonable. That is deliberately not a detailed policy: the situations it would
cover are not known in advance, and a policy written now would be wrong later.

---

*This document describes intent and responsibility. Legal terms are in
[LICENSE](LICENSE), which takes precedence where the two touch the same
subject.*
