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
; Assembled by CMakeLists.txt with armasm64.exe and linked into WAMR's vmlib.
; Copyright of the original: Intel Corporation, Apache-2.0 WITH LLVM-exception.
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
