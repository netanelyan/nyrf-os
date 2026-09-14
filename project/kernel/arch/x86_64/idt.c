/* Interrupt Descriptor Table - construction, loading, and the one C handler
 * every vector arrives at. See kernel/idt.h for why the kernel needs a table
 * of its own, and isr.asm for why the entry stubs cannot be written in C. */

#include <kernel/idt.h>
#include <kernel/gdt.h>
#include <kernel/tss.h>
#include <kernel/serial.h>

/* One IDT gate, 16 bytes in long mode - twice the width of a GDT entry,
 * because a handler address is 64 bits and there was nowhere left in the
 * original 8-byte layout to put the top half. The result is the same kind of
 * historical scattering the GDT entry suffers from: the offset arrives in
 * three pieces, low, middle and high, with the selector and the attributes
 * wedged between the first two.
 *
 *   bits   0-15   offset  0-15
 *   bits  16-31   code segment selector
 *   bits  32-34   IST index, 0 for "do not switch stacks"
 *   bits  35-39   zero
 *   bits  40-47   type and attributes
 *   bits  48-63   offset 16-31
 *   bits  64-95   offset 32-63
 *   bits  96-127  reserved, must be zero
 */
typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;       /* low 3 bits only; the rest must be zero */
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed)) idt_gate_t;

_Static_assert(sizeof(idt_gate_t) == 16, "an IDT gate must be exactly 16 bytes");

/* What lidt reads, identical in shape to the GDTR image: a 2-byte limit
 * followed by an 8-byte base, where the limit is the offset of the last valid
 * byte rather than the size. */
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_ptr_t;

_Static_assert(sizeof(idt_ptr_t) == 10, "the IDTR image must be 10 bytes");

/* -- type and attribute byte ------------------------------------------------
 *
 *   bit 7    P     present
 *   bits 6-5       DPL, the lowest privilege level allowed to enter this gate
 *                  through a software int instruction
 *   bit 4          zero, marking this a system descriptor rather than a
 *                  code or data segment
 *   bits 3-0       gate type
 *
 * The two gate types differ in one bit and in one behaviour: an interrupt gate
 * clears IF on entry, a trap gate leaves it alone. Interrupt gates throughout
 * here, which costs nothing while interrupts are masked anyway but is the
 * right default - a handler that can itself be interrupted needs to be
 * reentrant, and none of these are.
 *
 * DPL 0 means ring 3 cannot reach any of these with `int N`. That is what we
 * want for exceptions: a fault raised by the hardware is delivered regardless
 * of DPL, so ring 0 handling is unaffected, but userspace cannot forge a
 * page fault by executing `int 14`.
 */
#define GATE_PRESENT   0x80
#define GATE_DPL0      0x00
#define GATE_SYSTEM    0x00 /* bit 4 clear: not a code/data segment */
#define GATE_TYPE_INT  0x0E
#define GATE_TYPE_TRAP 0x0F

#define GATE_KERNEL_INT (GATE_PRESENT | GATE_DPL0 | GATE_SYSTEM | GATE_TYPE_INT) /* 0x8E */

/* Only the low three bits of the IST byte are the index; the rest are reserved
 * and must be zero. Masking rather than trusting the caller. */
#define GATE_IST_MASK 0x07

#define IDT_ENTRIES 256

/* Both must outlive idt_init(), for the same reason the GDT must outlive
 * gdt_init(): IDTR holds a pointer and the CPU keeps reading through it. A
 * local would be a stack frame that is already gone by the first fault.
 *
 * static also puts the table in .bss, which the ELF loader zero-fills - so the
 * reserved field of every gate is correct before a single one is written. */
static idt_gate_t idt[IDT_ENTRIES];
static idt_ptr_t  idtr;

/* kernel/arch/x86_64/isr.asm - one stub per exception vector, plus the shared
 * one for everything above 31.
 *
 * Declared individually and gathered into a table below rather than exported
 * as an array from the assembly, so that both halves stay obvious: the .asm
 * file defines 33 plain symbols, and the ordering that matters lives here in C
 * where it can be read against the vector numbers. */
extern void isr0(void),  isr1(void),  isr2(void),  isr3(void);
extern void isr4(void),  isr5(void),  isr6(void),  isr7(void);
extern void isr8(void),  isr9(void),  isr10(void), isr11(void);
extern void isr12(void), isr13(void), isr14(void), isr15(void);
extern void isr16(void), isr17(void), isr18(void), isr19(void);
extern void isr20(void), isr21(void), isr22(void), isr23(void);
extern void isr24(void), isr25(void), isr26(void), isr27(void);
extern void isr28(void), isr29(void), isr30(void), isr31(void);
extern void isr_unhandled(void);

static void (*const isr_stub[32])(void) = {
    isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
    isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
};

/* Indexed by vector. Kept in full, including the reserved slots, so that the
 * index is the vector and no arithmetic is needed to look one up. */
static const char *const exception_name[32] = {
    "#DE divide by zero",
    "#DB debug",
    "NMI non-maskable interrupt",
    "#BP breakpoint",
    "#OF overflow",
    "#BR bound range exceeded",
    "#UD invalid opcode",
    "#NM device not available",
    "#DF double fault",
    "coprocessor segment overrun",
    "#TS invalid TSS",
    "#NP segment not present",
    "#SS stack-segment fault",
    "#GP general protection fault",
    "#PF page fault",
    "reserved (15)",
    "#MF x87 floating-point error",
    "#AC alignment check",
    "#MC machine check",
    "#XM SIMD floating-point error",
    "#VE virtualisation exception",
    "#CP control protection exception",
    "reserved (22)",
    "reserved (23)",
    "reserved (24)",
    "reserved (25)",
    "reserved (26)",
    "reserved (27)",
    "#HV hypervisor injection exception",
    "#VC VMM communication exception",
    "#SX security exception",
    "reserved (31)",
};

/* Splits a handler address across the three offset fields and fills in the
 * rest. ist is 0 for every gate but the double fault. */
static void idt_set_gate(uint8_t vector, void (*handler)(void), uint8_t ist)
{
    const uint64_t offset = (uint64_t)(uintptr_t)handler;

    idt[vector].offset_low  = (uint16_t)(offset & 0xFFFF);
    idt[vector].selector    = GDT_SEL_KERNEL_CODE;
    idt[vector].ist         = (uint8_t)(ist & GATE_IST_MASK);
    idt[vector].type_attr   = GATE_KERNEL_INT;
    idt[vector].offset_mid  = (uint16_t)((offset >> 16) & 0xFFFF);
    idt[vector].offset_high = (uint32_t)(offset >> 32);
    idt[vector].reserved    = 0;
}

void idt_init(void)
{
    /* Before anything else: gate 8 is about to name IST slot 1, and a gate
     * referring to an IST slot that no loaded TSS provides is a fault on
     * delivery rather than a handled exception. The TSS has to be live first.
     *
     * This is also why kernel_main calls only gdt_init() and idt_init() - the
     * dependency belongs here, where it is visible, rather than as a third
     * call in kernel_main that has to be kept in the right order by hand. */
    tss_init();

    /* Every vector gets a handler, including the 224 that cannot fire yet.
     * An unfilled gate is not inert: it is a not-present gate, so touching it
     * raises #GP, and a #GP whose own gate is missing escalates. Pointing the
     * lot at one stub costs nothing and makes the table total. */
    for (int vector = 0; vector < IDT_ENTRIES; vector++) {
        idt_set_gate((uint8_t)vector, isr_unhandled, 0);
    }

    /* Then the 32 that have real stubs, overwriting the shared one. */
    for (int vector = 0; vector < 32; vector++) {
        idt_set_gate((uint8_t)vector, isr_stub[vector], 0);
    }

    /* And the double fault again, this time on its own stack. See kernel/tss.h
     * for why this one vector is special. */
    idt_set_gate(IDT_VECTOR_DOUBLE_FAULT, isr_stub[IDT_VECTOR_DOUBLE_FAULT],
                 TSS_IST_DOUBLE_FAULT);

    idtr.limit = (uint16_t)(sizeof(idt) - 1);
    idtr.base  = (uint64_t)(uintptr_t)idt;

    /* "m" rather than "r": lidt takes a memory operand, the 10-byte image
     * itself, not its address in a register. */
    __asm__ __volatile__("lidt %0" : : "m"(idtr));

    serial_printf("[idt]   table at %x, %u entries, limit=%u\n",
                  idtr.base, (uint64_t)IDT_ENTRIES, (uint64_t)idtr.limit);
    serial_printf("[idt]   vectors 0-31 have stubs, 32-255 shared; gate %u on IST%u\n",
                  (uint64_t)IDT_VECTOR_DOUBLE_FAULT,
                  (uint64_t)TSS_IST_DOUBLE_FAULT);
}

/* Every vector lands here. Prints what happened and stops.
 *
 * Nothing is recoverable at this stage of the kernel: there is no scheduler to
 * kill a task, no paging to fix up a fault, and no userspace to blame. So the
 * only useful thing a handler can do is describe the machine's state precisely
 * enough to find the bug from the log, then halt before anything overwrites
 * the evidence.
 *
 * Every argument is cast to uint64_t because serial_printf reads exactly that
 * width for %x and %u and cannot check its own format string - the struct
 * fields are already uint64_t, so the casts appear only where the value is
 * not. */
void exception_dispatch(exception_frame_t *frame)
{
    const char *name = (frame->vector < 32)
                           ? exception_name[frame->vector]
                           : "unknown vector (shared stub)";

    serial_puts("\n[idt]   *** CPU EXCEPTION - halting ***\n");
    serial_printf("[idt]   vector=%u  %s\n", frame->vector, name);
    serial_printf("[idt]   error=%x\n", frame->error_code);

    /* CR2 holds the linear address whose access faulted, and is meaningful
     * only for a page fault. It is a register rather than part of the frame,
     * so it has to be read here - and read before anything else touches
     * memory in a way that could fault again and overwrite it. */
    if (frame->vector == IDT_VECTOR_PAGE_FAULT) {
        uint64_t cr2 = 0;
        __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
        serial_printf("[idt]   CR2=%x  (the address that faulted)\n", cr2);
    }

    serial_printf("[idt]   RIP=%x CS=%x\n", frame->rip, frame->cs);
    serial_printf("[idt]   RSP=%x SS=%x\n", frame->rsp, frame->ss);
    serial_printf("[idt]   RFLAGS=%x\n", frame->rflags);

    serial_printf("[idt]   RAX=%x RBX=%x RCX=%x\n",
                  frame->rax, frame->rbx, frame->rcx);
    serial_printf("[idt]   RDX=%x RSI=%x RDI=%x\n",
                  frame->rdx, frame->rsi, frame->rdi);
    serial_printf("[idt]   RBP=%x R8 =%x R9 =%x\n",
                  frame->rbp, frame->r8, frame->r9);
    serial_printf("[idt]   R10=%x R11=%x R12=%x\n",
                  frame->r10, frame->r11, frame->r12);
    serial_printf("[idt]   R13=%x R14=%x R15=%x\n",
                  frame->r13, frame->r14, frame->r15);

    serial_puts("[idt]   halted.\n");

    /* cli as well as hlt: interrupts are masked now, but this handler is the
     * one piece of the kernel that must stay stopped even if something later
     * unmasks them. */
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}
