;
; invokeNative for Windows on Arm64, in armasm64 (MSVC) syntax.
;
; WHY THIS FILE EXISTS. WAMR calls every host function a mod imports through
; invokeNative, an assembly trampoline that loads argv into the argument
; registers. It ships one per architecture, but for AArch64 only as GAS syntax
; (core/iwasm/common/arch/invokeNative_aarch64.s), which MSVC's armasm64 cannot
; read -- so an MSVC Arm64 build of the mod runtime had no invokeNative at all
; and failed at link. The configure used to stop there with a FATAL_ERROR.
;
; This is the same routine, instruction for instruction, translated to armasm64.
; It is correct on Windows because Windows on Arm64 follows AAPCS64 for every
; non-variadic call: x0-x7 integer arguments, d0-d7 floating point, the rest on
; a 16-byte-aligned stack in 8-byte slots, x19-x28 callee-saved. The trampoline
; never touches x18, which Windows reserves for the TEB.
;
; NO UNWIND INFO, deliberately matching WAMR's own x64 MASM trampoline
; (invokeNative_em64.asm, a plain PROC), which the shipped Windows x64 build has
; always run without it. An exception raised INSIDE a host function and caught
; above the interpreter is the only unwind that crosses this frame, and the two
; architectures now carry the same exposure to it rather than different ones.
;
; Arguments:  x0 function pointer   x1 argv   x2 number of stack arguments
;
; argv layout, set by wasm_runtime_invoke_native for AArch64:
;   argv[0..7]   -> d0-d7
;   argv[8..15]  -> x0-x7   (argv[8] is the exec_env)
;   argv[16..]   -> stack
;
; ONLY WITH cmake/wamr_windows_arm64_abi.cmake APPLIED. Upstream WAMR packs the
; buffer with the Windows x64 rules (4+4 registers, by position) on every
; _WIN32, Arm64 included; unpatched, argv[8] is the first stack argument and
; every host call that reads its exec_env crashes. The patch makes Windows on
; Arm64 pack for AAPCS64, which is what this routine and Windows both expect.
; That patch is NOT optional and cannot be skipped silently: it is a mandatory
; PATCH_COMMAND on the WAMR fetch (CMakeLists.txt), and the script FAILS THE
; CONFIGURE (FATAL_ERROR) if the text it edits has moved -- so there is no way to
; build a Windows-on-Arm64 mod runtime with the x64 packing and this unpacker.
; A subtler mismatch (patched but wrong) would make self(e) return the wrong or
; no instance, which ModCapabilityTest catches wherever it runs on Arm64.
;
; Assembled by CMakeLists.txt with armasm64.exe and linked into WAMR's vmlib.
; Copyright of the original: Intel Corporation, Apache-2.0 WITH LLVM-exception.
;
; AUDITED 2026-10 against AAPCS64 and the GAS original, register by register.
; A mod cannot drive this with a crafted signature: the signature string is the
; HOST's (from the NativeSymbol table registerNatives builds), WAMR validates a
; mod's import type against it at link, and wasm_runtime_invoke_native -- not the
; mod -- computes (funcptr, argv, nStackArgs). The trampoline only ever sees
; integers. Points checked:
;   * x0-x7 / d0-d7 loaded from argv[8..15] / argv[0..7]; x20 then at argv[16],
;     the stack args, which is the layout wasm_runtime_invoke_native packs.
;   * sp is 16-byte aligned at every `blr`: x22 (sp after the 0x30 prologue) is
;     aligned because entry sp is, 0x30 is a multiple of 16, and the stack-arg
;     reservation rounds its size up to 16 before subtracting.
;   * lr is preserved across the call in x20, which is sound ONLY because x20 is
;     callee-saved under AAPCS64, so the callee must restore it; x19-x24 are
;     saved/restored for the caller, x18 (the Windows TEB) is never touched.
;   * the stack-arg copy writes below sp into space this frame reserved, never
;     over the saved registers.
; The stack-argument path (argv[16..], nStackArgs > 0) is the subtle half, and it
; IS reachable. gearbox:ui.draw_image ("(iiiiiiii)") declares eight slots, which
; with the exec_env in x0 spills one past x0-x7 onto the stack, and
; ModCapabilityTest calls every gearbox import once -- so on a runner using this
; trampoline it drives the stack path, not only the register one. (The widest of
; all is the denied WASI stub path_open, "(iiiiiIIii)i", nine slots spilling two.)
; The asm itself was verified by inspection above because no CI runner is
; Windows-on-Arm64 today.
;

        AREA    |.text|, CODE, READONLY, ALIGN=2

        EXPORT  invokeNative

invokeNative PROC
        sub     sp, sp, #0x30
        stp     x19, x20, [sp, #0x20]   ; save the callee-saved registers we use
        stp     x21, x22, [sp, #0x10]
        stp     x23, x24, [sp, #0x0]

        mov     x19, x0                 ; x19 = function pointer
        mov     x20, x1                 ; x20 = argv
        mov     x21, x2                 ; x21 = number of stack arguments
        mov     x22, sp                 ; sp before the call, to restore after

        ; floating-point argument registers
        ldp     d0, d1, [x20], #16
        ldp     d2, d3, [x20], #16
        ldp     d4, d5, [x20], #16
        ldp     d6, d7, [x20], #16

        ; integer argument registers (x0 = argv[8] = exec_env)
        ldp     x0, x1, [x20], #16
        ldp     x2, x3, [x20], #16
        ldp     x4, x5, [x20], #16
        ldp     x6, x7, [x20], #16

        ; x20 now points at the stack arguments
        cmp     x21, #0
        beq     call_func

        ; reserve 16-byte-aligned stack space and copy the stack arguments in
        mov     x23, sp
        bic     sp, x23, #15
        lsl     x23, x21, #3
        add     x23, x23, #15
        bic     x23, x23, #15
        sub     sp, sp, x23
        mov     x23, sp

loop_stack_args
        cmp     x21, #0
        beq     call_func
        ldr     x24, [x20], #8
        str     x24, [x23], #8
        sub     x21, x21, #1
        b       loop_stack_args

call_func
        mov     x20, x30                ; save lr
        blr     x19
        mov     sp, x22                 ; restore sp

        mov     x30, x20                ; restore lr
        ldp     x19, x20, [sp, #0x20]
        ldp     x21, x22, [sp, #0x10]
        ldp     x23, x24, [sp, #0x0]
        add     sp, sp, #0x30
        ret
        ENDP

        END
