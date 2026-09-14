; nyrf OS kernel entry point.
;
; The bootloader jumps here with a boot_info_t pointer already in RDI, using
; the System V convention. All this stub does is give the kernel a stack of its
; own - the firmware stack is inside memory the kernel is free to reclaim - and
; hand control to C.
;
; It exists in assembly because none of it can be expressed in C: a C function
; needs a working stack before its first instruction, so whatever establishes
; that stack cannot itself be one. This is the only file in the kernel that is
; not C, and it is deliberately the smallest it can be - everything that *can*
; move into kernel_main has.

; The CPU is already in 64-bit long mode when we arrive: UEFI firmware enters
; its applications that way, so there is none of the real-mode-to-protected-
; to-long-mode ceremony a BIOS bootloader would need here. Paging is on too,
; identity-mapped by the firmware, which is what lets a kernel linked to
; physical 0x100000 simply run.
bits 64

; Its own section so the linker script can put it first: _start then sits at
; exactly KERNEL_PHYS_BASE, which makes the "is RIP even in the kernel?" check
; of milestone M5 a single glance in GDB.
;
; The flags spell out what would otherwise be inferred from the section name:
; progbits = occupies space in the file, alloc = gets loaded into memory,
; exec = executable, nowrite = read-only. NASM will not guess these for a
; custom section name, so they have to be stated.
section .text.entry progbits alloc exec nowrite align=16

; global exports _start so the linker script's ENTRY() can find it; extern
; declares kernel_main as somebody else's symbol, resolved at link time.
global _start
extern kernel_main

_start:
    cli                         ; no IDT yet, so no interrupt may fire
    cld                         ; System V requires DF clear on entry to C

    ; Switch off the firmware's stack and onto our own, before anything is
    ; pushed. lea with rel gives a RIP-relative address, which is both smaller
    ; and correct regardless of where the code sits - the habit to keep even
    ; though this kernel is linked to a fixed address.
    ;
    ; Note it points at stack_top, not stack_bottom: x86 stacks grow downwards,
    ; so the top of the reserved block is the *empty* end.
    lea rsp, [rel stack_top]
    xor rbp, rbp                ; terminate the frame chain for the debugger

    ; RDI has been carried untouched all the way from the bootloader's call -
    ; nothing above clobbers it - so the C function's first argument is already
    ; in place and there is nothing to marshal.
    call kernel_main            ; RDI still holds boot_info_t*

.hang:                          ; kernel_main is not supposed to return
    hlt
    jmp .hang                   ; hlt resumes on NMI, so loop around it

; .bss rather than .data: the stack has no initial contents worth storing, and
; putting it here costs nothing in the kernel image. It is the reason the
; kernel's PT_LOAD segment has memsz > filesz, and therefore the reason
; boot/elf.c has to zero-fill the tail.
section .bss
align 16                        ; System V wants RSP 16-byte aligned at a call
stack_bottom:
    resb 16 * 1024              ; 16 KiB is plenty for a kernel that only draws
stack_top:
