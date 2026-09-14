; Load the GDT and reload every segment register.
;
; Split out of gdt.c because reloading CS cannot be written in C at all, and
; expressing it as inline asm hides the one instruction that matters.

bits 64

section .text

global gdt_flush

; void gdt_flush(const gdt_ptr_t *ptr, uint16_t code_sel, uint16_t data_sel)
;
; System V puts the arguments in RDI, RSI, RDX. Only the low 16 bits of RSI
; and RDX carry a selector.
gdt_flush:
    lgdt [rdi]                  ; GDTR now points at our table

    ; The data segment registers. In long mode their contents are almost
    ; entirely ignored, but they must still hold a valid selector or null,
    ; and they currently hold the firmware's.
    mov ax, dx
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; CS is the one that cannot be written with mov. There is no instruction
    ; that loads it directly; it only changes as a side effect of a control
    ; transfer that carries a selector. A far return is the cheapest one.
    ;
    ; retfq pops RIP first and CS second, so the frame is built in the
    ; opposite order: selector pushed first, target address pushed second.
    movzx rax, si               ; the code selector, zero-extended to 64 bits
    push rax                    ; -> popped second, becomes CS
    lea  rax, [rel .reloaded]
    push rax                    ; -> popped first,  becomes RIP
    retfq                       ; the q matters: 64-bit operands, not 32

.reloaded:                      ; first instruction running under our own CS
    ret

; Tells the linker the stack need not be executable. Without it ld warns on
; every build.
section .note.GNU-stack noalloc noexec nowrite progbits
