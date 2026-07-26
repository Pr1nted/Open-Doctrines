/* gbx_throw.h — Lua error handling without setjmp/longjmp.
 *
 * Force-included into every Lua translation unit (see build.sh: -include).
 *
 * WHY THIS EXISTS
 *
 * Lua signals errors by longjmp-ing out to the nearest lua_pcall. That does not
 * work on wasm32: there is no stack to unwind. The usual fix is to compile with
 * -fwasm-exceptions and turn on WAMR's exception-handling proposal -- but in
 * WAMR 2.4.5 the try/catch/throw opcodes are implemented ONLY in the classic
 * interpreter. wasm_interp_fast.c raises "unsupported opcode" for them even when
 * WASM_ENABLE_EXCE_HANDLING=1. Enabling exceptions therefore means setting
 * WAMR_BUILD_FAST_INTERP=0, which slows down every mod in every other language
 * to buy error recovery for this one.
 *
 * ldo.c gates its definitions behind `#if !defined(LUAI_THROW)`, so we take the
 * documented escape hatch and define them first:
 *
 *   LUAI_THROW  ->  log the error, then trap the instance.
 *   LUAI_TRY    ->  run the body unprotected.
 *
 * WHAT THIS COSTS, STATED PLAINLY
 *
 * pcall and xpcall no longer recover. A Lua error terminates the mod instead of
 * returning an error code to the script. The host already treats a trapped
 * instance as a disabled mod with a diagnostic, so the failure is reported
 * rather than silent -- but a script CANNOT catch its own errors, and any Lua
 * library that relies on pcall for control flow will take the mod down with it.
 *
 * This is a deliberate trade: correct-or-dead for Lua mods, full speed for
 * everyone else. If WAMR ever implements exception handling in the fast
 * interpreter, delete this file and build with -fwasm-exceptions instead.
 */

#ifndef GBX_THROW_H
#define GBX_THROW_H

struct lua_State;

/* Defined in gearbox_lua.c. Logs the pending error through the Core log
 * import, then executes an unreachable. Never returns. */
__attribute__((noreturn))
void gbx_lua_throw(struct lua_State *L, int errcode);

#define LUAI_THROW(L, c)    gbx_lua_throw((L), (int)(c)->status)
#define LUAI_TRY(L, c, a)   { a }
#define luai_jmpbuf         int   /* unused; kept so lua_longjmp still compiles */

#endif /* GBX_THROW_H */
