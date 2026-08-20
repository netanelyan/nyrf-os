; nyrf OS kernel entry point.
;
; The bootloader jumps here with a boot_info_t pointer already in RDI, using
; the System V convention. All this stub does is give the kernel a stack of its
; own - the firmware stack is inside memory the kernel is free to reclaim - and
; hand control to C.

bits 64

; Its own section so the linker script can put it first: _start then sits at
; exactly KERNEL_PHYS_BASE, which makes the "is RIP even in the kernel?" check
; of milestone M5 a single glance in GDB.
section .text.entry progbits alloc exec nowrite align=16

global _start
extern kernel_main

_start:
    cli                         ; no IDT yet, so no interrupt may fire
    cld                         ; System V requires DF clear on entry to C
    lea rsp, [rel stack_top]
    xor rbp, rbp                ; terminate the frame chain for the debugger

    call kernel_main            ; RDI still holds boot_info_t*

.hang:                          ; kernel_main is not supposed to return
    hlt
    jmp .hang

section .bss
align 16
stack_bottom:
    resb 16 * 1024              ; 16 KiB is plenty for a kernel that only draws
stack_top:
