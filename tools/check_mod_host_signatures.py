#!/usr/bin/env python3
"""Every mod host native's WAMR signature string matches its C function.

    tools/check_mod_host_signatures.py

WHY THIS EXISTS

WAMR uses the signature string in the registration table -- "(iii)i" and such --
to pack a mod's arguments into registers and the stack before the asm trampoline
jumps to the C function. If that string disagrees with the function's ACTUAL
parameters, the trampoline loads the wrong registers or reads argv slots that
were never written: a host-side memory-safety bug, not something a mod can aim,
but a latent crash or worse.

One such typo already shipped: an entry wrote "(iiiF)i" where the function took
an i32 where the F said f64 (the ABI alphabet is i/I/f/F -- there is no 'd').
ModAbiTest compared the table against sdk/abi.json and found them in perfect
agreement, because the SAME typo was in both. Nothing that reads a table could
catch it; it took calling the function. This reads the C function instead.

THE MAPPING. A parameter's declared type fixes its signature letter:
    uint32_t / int32_t / a pointer (passed as an i32 offset)  -> i
    uint64_t / int64_t                                         -> I
    float                                                      -> f
    double                                                     -> F
and the return type the same, with void meaning no letter after ')'. The first
parameter is always ExecEnv (the exec env WAMR prepends) and is not in the
signature.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST_CPP = os.path.join(ROOT, "src", "mods", "ModHost.cpp")

# C type -> signature letter. Pointers are declared uint32_t here (every pointer
# crosses the ABI as an i32 linear-memory offset the host validates itself), so
# they fall under i with the rest.
TYPE_LETTER = {
    "uint32_t": "i", "int32_t": "i", "int": "i", "unsigned": "i",
    "uint64_t": "I", "int64_t": "I", "long": "I", "size_t": "i",
    "float": "f",
    "double": "F",
}
RET_LETTER = dict(TYPE_LETTER)
RET_LETTER["void"] = ""


def strip_cxx(src: str) -> str:
    out, i, n = [], 0, len(src)
    while i < n:
        c, two = src[i], src[i:i + 2]
        if two == "//":
            while i < n and src[i] != "\n":
                i += 1
        elif two == "/*":
            i += 2
            while i < n and src[i:i + 2] != "*/":
                out.append("\n" if src[i] == "\n" else " ")
                i += 1
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def type_to_letter(decl: str, table):
    """Map a C parameter/return declaration to its signature letter, or None if
    the type is not one the ABI can carry."""
    toks = decl.replace("*", " ").split()
    toks = [t for t in toks if t not in ("const", "unsigned", "volatile")]
    # unsigned was stripped; re-add its effect by checking the original.
    base = None
    for t in toks:
        if t in table:
            base = t
            break
    if base is None:
        # e.g. "unsigned int" collapsed, or a bare "unsigned".
        if "unsigned" in decl or "int" in decl:
            return "i"
        return None
    return table[base]


def parse_functions(src: str):
    """fnname -> (arg_letters, ret_letter) for every `<ret> name(ExecEnv e...)`."""
    funcs = {}
    # Macro-generated natives the regex below cannot see. OD_TREATY_THUNK expands
    # to `uint32_t fn(ExecEnv e, uint32_t a, uint32_t b) { ... }`, i.e. "(ii)i".
    for m in re.finditer(r"OD_TREATY_THUNK\s*\(\s*([A-Za-z_]\w*)", src):
        funcs[m.group(1)] = ("ii", "i")
    for m in re.finditer(r"\b([A-Za-z_][\w:\s\*]*?)\b([A-Za-z_]\w*)\s*\(\s*ExecEnv\s+e\b",
                         src):
        ret_decl = m.group(1).strip()
        name = m.group(2)
        # Grab the full parameter list by paren-matching from the '('.
        paren = src.index("(", m.start())
        depth, i, n = 0, paren, len(src)
        while i < n:
            if src[i] == "(":
                depth += 1
            elif src[i] == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        params = src[paren + 1:i]
        # Only a definition (has a body) or a plain forward decl both work; we
        # just need the signature. Skip if this is actually a call, not a decl:
        # a decl's ret_decl is a type, a call has none.
        if not ret_decl:
            continue
        ret = type_to_letter(ret_decl, RET_LETTER)
        parts = [p.strip() for p in params.split(",")]
        parts = parts[1:]  # drop ExecEnv e
        args = []
        ok = True
        for p in parts:
            if not p:
                continue
            letter = type_to_letter(p, TYPE_LETTER)
            if letter is None:
                ok = False
                break
            args.append(letter)
        if ok and ret is not None:
            funcs[name] = ("".join(a for a in args), ret if ret is not None else "")
    return funcs


def parse_table(src: str):
    """Yield (module, name, sig, fn) for each registration row."""
    row = re.compile(
        r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"(\([iIfF]*\)[iIfF]?)"\s*,'
        r'\s*(?:\(void\s*\*\)\s*)?([A-Za-z_]\w*)\s*,')
    for m in row.finditer(src):
        yield m.group(1), m.group(2), m.group(3), m.group(4)


def main() -> int:
    with open(HOST_CPP, encoding="utf-8") as f:
        src = strip_cxx(f.read())

    funcs = parse_functions(src)
    rows = list(parse_table(src))
    if not rows:
        print("FAIL  found no registration rows -- the table or parser changed shape")
        return 1

    problems = []
    checked = 0
    for module, name, sig, fn in rows:
        m = re.match(r"\(([iIfF]*)\)([iIfF]?)", sig)
        sig_args, sig_ret = m.group(1), m.group(2)
        if fn not in funcs:
            problems.append(f"{module}.{name}: row names {fn}(), which has no "
                            f"ExecEnv definition this checker can read")
            continue
        c_args, c_ret = funcs[fn]
        checked += 1
        # The safety rule is one-directional: the C function must not READ more
        # (or differently-typed) arguments than the signature marshals. Taking
        # fewer is fine and deliberate -- the arity-agnostic WASI denial stubs
        # (wasi_denied2 used for a 4-arg fd_read, etc.) read none of theirs. So
        # the function's letters must be a PREFIX of the signature's: equal for a
        # real function, shorter for a stub, and any type mismatch at a read
        # position (the historical "(iiiF)" where the arg was an i32) fails.
        if len(c_args) > len(sig_args) or sig_args[:len(c_args)] != c_args:
            problems.append(
                f'{module}.{name} ({fn}): signature args "{sig_args}" but the C '
                f'function reads "{c_args}"')
        if sig_ret != c_ret:
            problems.append(
                f'{module}.{name} ({fn}): signature returns "{sig_ret or "void"}" '
                f'but the C function returns "{c_ret or "void"}"')

    if problems:
        for p in problems:
            print(f"FAIL  {p}")
        print(f"\n{len(problems)} signature mismatch(es). The WAMR signature "
              f"string (i=i32, I=i64, f=f32, F=f64) must match the C function, or "
              f"the trampoline marshals arguments wrong.")
        return 1

    print(f"ok    all {checked} mod host signatures match their C functions "
          f"(i/I/f/F)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
