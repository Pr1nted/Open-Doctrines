#pragma once

#include <cstdlib>

/**
 * getenv(), answered once per call site.
 *
 * WHY. The environment is a flat list and getenv walks it under a lock --
 * __findenv_locked on macOS. That is fine at startup and not fine in a loop
 * that runs per country per turn, which is where the A/B gates ended up: every
 * experiment leaves its switch behind, and by October a profile of processTurn
 * had __findenv_locked as its fourth-hottest entry at 4.6% of all samples,
 * ahead of most of the game's own work.
 *
 * MEASURED, and the measurement is the fun part: adding 300 unrelated
 * variables to the environment made turns 48.6% slower (0.0275 s -> 0.0409 s,
 * three runs each, spread under 0.2%). A game whose turn time depends on how
 * many environment variables the shell happens to export is reading them
 * somewhere it should not.
 *
 * The lambda's static is initialised once, thread-safely, and every call after
 * the first is a guard check and a load. It is a drop-in for getenv in any
 * expression -- `if (OD_ENV("X"))`, `atoi(OD_ENV("X"))` -- so the call sites
 * keep their shape.
 *
 * ONLY FOR A STRING LITERAL, and only for a switch that is read rather than
 * written: it answers what the variable said the FIRST time this line ran, so
 * anything setenv() changes mid-run must keep using getenv directly.
 */
#define OD_ENV(name) \
    ([]() -> const char* { static const char* v = std::getenv(name); return v; }())
