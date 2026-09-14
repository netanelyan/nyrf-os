/* Interrupt Descriptor Table.
 *
 * The IDT is the CPU's jump table for everything that interrupts normal
 * execution: faults raised by an instruction, traps like int3, non-maskable
 * interrupts, and later the hardware IRQs the APIC delivers. Each of its 256
 * entries is a gate naming a handler address and the privilege to enter it
 * with.
 *
 * Until idt_init() runs, the kernel is living on the IDT the UEFI firmware
 * built - or, after gdt_init() reloaded the segment registers, on one whose
 * code selector no longer means what the firmware intended. Either way the
 * first fault would be unrecoverable and silent: no handler, no message, and
 * then a triple fault resetting the machine. Installing the table turns every
 * such fault into a register dump on the serial line instead.
 *
 * This step is CPU exceptions only. Interrupts stay masked - there is no sti
 * anywhere, and no PIC or APIC has been touched - so nothing above vector 31
 * can fire yet.
 */
#ifndef NYRF_IDT_H
#define NYRF_IDT_H

#include <stdint.h>
#include <stddef.h>

/* The vectors this kernel names explicitly. The rest are identified by number
 * and by the table in idt.c. */
#define IDT_VECTOR_DIVIDE_ERROR 0
#define IDT_VECTOR_DOUBLE_FAULT 8
#define IDT_VECTOR_PAGE_FAULT   14

/* Vector pushed by the shared stub, which cannot know its own number. Chosen
 * as 0xFF because it is outside the exception range and so cannot collide with
 * a real one. */
#define IDT_VECTOR_UNKNOWN      0xFF

/* Everything the stubs in isr.asm leave on the stack, in the order it lands
 * there. This structure is not a hardware layout but an ABI between that file
 * and this one, and it is just as unforgiving: the field order here has to
 * mirror the push order there exactly, in reverse.
 *
 * Reading it from the top down is reading the stack from its lowest address
 * upwards. isr_common pushes RAX first and R15 last, so R15 sits lowest and
 * comes first here. Above the registers sit the vector and error code the stub
 * pushed, and above those the frame the CPU itself pushed on entry.
 *
 * In long mode the CPU always pushes SS and RSP, even when the interrupt does
 * not change privilege level - unlike 32-bit protected mode, where they appear
 * only on a ring change. That is why rsp and ss can be read unconditionally.
 */
typedef struct {
    /* Pushed by isr_common, in reverse of this order. */
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;

    /* Pushed by the per-vector stub. */
    uint64_t vector;
    uint64_t error_code; /* the CPU's on eight vectors, a dummy 0 on the rest */

    /* Pushed by the CPU. */
    uint64_t rip;        /* the faulting instruction, or the one after it */
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} exception_frame_t;

/* Asserted rather than trusted, because nothing else would catch a mismatch:
 * adding a field here without adding a push there produces a handler that
 * prints plausible-looking nonsense, shifted by one register. The offsets pin
 * the three boundaries that matter - where the registers start, where the
 * stub's own pushes begin, and where the CPU's frame begins. */
_Static_assert(sizeof(exception_frame_t) == 176, "exception_frame_t must match the push order in isr.asm");
_Static_assert(offsetof(exception_frame_t, r15) == 0, "r15 must be pushed last");
_Static_assert(offsetof(exception_frame_t, rax) == 112, "rax must be pushed first");
_Static_assert(offsetof(exception_frame_t, vector) == 120, "vector follows the saved registers");
_Static_assert(offsetof(exception_frame_t, rip) == 136, "the CPU frame follows the error code");
_Static_assert(offsetof(exception_frame_t, ss) == 168, "ss is the top of the CPU frame");

/* Builds the 256-entry table, points every vector at a stub, and loads it with
 * lidt. Also brings up the TSS first, because gate 8 refers to an IST slot and
 * that slot has to exist before the gate can name it. */
void idt_init(void);

/* The single C entry point for every vector. Called from isr_common in
 * isr.asm with a pointer to the frame described above; declared here because
 * that file and this structure are two halves of one contract. Never
 * returns. */
void exception_dispatch(exception_frame_t *frame);

#endif /* NYRF_IDT_H */
