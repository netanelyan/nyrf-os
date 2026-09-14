; CPU exception entry stubs for vectors 0..31, plus one shared stub for
; everything above them.
;
; Why this cannot be C. A handler entered by the CPU is not a function call:
; nothing has saved the caller's registers, the return is iret rather than ret,
; and on some vectors there is an extra word on the stack that on others is not
; there. A C function's prologue would clobber registers before we ever got to
; look at them, and C has no way to say "the stack already holds a frame in
; this exact shape". So the job of this file is to turn an interrupt into
; something that *is* a normal C call: save every register, make the two stack
; shapes identical, and hand the result over as one pointer.
;
; The error code inconsistency. Eight vectors - 8, 10, 11, 12, 13, 14, 17 and
; 21 - make the CPU push a 64-bit error code after the interrupt frame; the
; rest do not. That is not a rule with a reason so much as an accident of which
; faults happened to need extra information when each was introduced, frozen
; into the architecture forever. It means two different stack layouts arrive at
; the same handler, so the no-error stubs push a dummy zero in the CPU's place.
; After that one push, every vector looks the same from C's point of view, and
; exception_dispatch needs no special cases at all.
;
; Every stub then pushes its own vector number, because once control reaches
; shared code there is no register or stack word that says which vector fired.
; That is also why vectors 0..31 get 32 nearly identical stubs instead of one:
; the vector number has to be baked in by the only thing that knows it, which
; is the entry point itself.

bits 64

section .text

; kernel/arch/x86_64/idt.c
extern exception_dispatch

; -- the two stub shapes ----------------------------------------------------
;
; The push of the vector is deliberately last, so that it ends up adjacent to
; the error code and the pair reads as two consecutive fields in
; exception_frame_t. See idt.h for the full layout.

%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push 0                      ; dummy error code, standing in for the CPU's
    push %1                     ; vector number
    jmp isr_common
%endmacro

%macro ISR_ERRCODE 1
global isr%1
isr%1:
    ; no dummy here: the CPU has already pushed a real error code
    push %1                     ; vector number
    jmp isr_common
%endmacro

ISR_NOERRCODE 0                 ; #DE  divide by zero
ISR_NOERRCODE 1                 ; #DB  debug
ISR_NOERRCODE 2                 ;      non-maskable interrupt
ISR_NOERRCODE 3                 ; #BP  breakpoint
ISR_NOERRCODE 4                 ; #OF  overflow
ISR_NOERRCODE 5                 ; #BR  bound range exceeded
ISR_NOERRCODE 6                 ; #UD  invalid opcode
ISR_NOERRCODE 7                 ; #NM  device not available
ISR_ERRCODE   8                 ; #DF  double fault
ISR_NOERRCODE 9                 ;      coprocessor segment overrun (obsolete)
ISR_ERRCODE   10                ; #TS  invalid TSS
ISR_ERRCODE   11                ; #NP  segment not present
ISR_ERRCODE   12                ; #SS  stack-segment fault
ISR_ERRCODE   13                ; #GP  general protection fault
ISR_ERRCODE   14                ; #PF  page fault
ISR_NOERRCODE 15                ;      reserved
ISR_NOERRCODE 16                ; #MF  x87 floating-point
ISR_ERRCODE   17                ; #AC  alignment check
ISR_NOERRCODE 18                ; #MC  machine check
ISR_NOERRCODE 19                ; #XM  SIMD floating-point
ISR_NOERRCODE 20                ; #VE  virtualisation
ISR_ERRCODE   21                ; #CP  control protection
ISR_NOERRCODE 22                ;      reserved
ISR_NOERRCODE 23                ;      reserved
ISR_NOERRCODE 24                ;      reserved
ISR_NOERRCODE 25                ;      reserved
ISR_NOERRCODE 26                ;      reserved
ISR_NOERRCODE 27                ;      reserved
ISR_NOERRCODE 28                ; #HV  hypervisor injection
ISR_NOERRCODE 29                ; #VC  VMM communication
ISR_NOERRCODE 30                ; #SX  security exception
ISR_NOERRCODE 31                ;      reserved

; Vectors 32..255 all share this one. It pushes 0xFF rather than a real vector
; number, because a shared stub genuinely cannot tell which vector reached it -
; the reason each exception above has its own. Nothing should reach this until
; the APIC is wired up; if it fires now, something is very wrong and saying so
; is more useful than saying which.
global isr_unhandled
isr_unhandled:
    push 0                      ; dummy error code
    push 0xFF                   ; "vector unknown", see idt.c
    jmp isr_common

; -- the common path --------------------------------------------------------
;
; On entry the stack holds, from higher addresses down: the CPU's frame (SS,
; RSP, RFLAGS, CS, RIP), the error code, and the vector. Pushing the fifteen
; general-purpose registers below that completes exactly the layout
; exception_frame_t describes - which is why they are pushed in reverse field
; order, RAX first so it lands highest and R15 last so it lands lowest.
;
; RSP is 16-byte aligned at the call. The CPU aligns the stack before pushing
; its frame, and the frame plus error code plus vector plus fifteen registers
; comes to a multiple of 16 either way, so the System V requirement is met
; without an explicit adjustment. Nothing here uses SSE, so this is hygiene
; rather than a crash waiting to happen - but it costs nothing to be right.

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; An interrupt gate clears IF and TF, but it leaves DF exactly as the
    ; faulting code had it, and System V requires DF clear on entry to a C
    ; function. entry.asm clears it once at boot, so in practice it is already
    ; clear - but "in practice" is not the same as guaranteed, and a string
    ; instruction running backwards inside the handler would be a miserable
    ; thing to debug.
    cld

    ; The frame we just built *is* the argument. RSP is its lowest address,
    ; which is the address of its first field.
    mov rdi, rsp
    call exception_dispatch

    ; exception_dispatch never returns. If it somehow did, stop here rather
    ; than unwinding and iret-ing back to the faulting instruction, which
    ; would fault again immediately and loop forever.
.hang:
    cli
    hlt
    jmp .hang

; Tells the linker the stack need not be executable. Without it ld warns on
; every build.
section .note.GNU-stack noalloc noexec nowrite progbits
