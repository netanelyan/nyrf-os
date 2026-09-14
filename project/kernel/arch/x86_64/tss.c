/* Task State Segment - construction and loading. See kernel/tss.h for what a
 * TSS still does in long mode and why the double fault needs one. */

#include <kernel/tss.h>
#include <kernel/gdt.h>
#include <kernel/serial.h>

#include <stddef.h>

/* The long-mode TSS, 104 bytes.
 *
 * Almost all of it is reserved or unused: the fields the 386 needed for
 * hardware task switching are still nailed into the layout, now permanently
 * zero. Only the stack pointers and iomap_base mean anything here.
 *
 * The seven IST entries are one array rather than seven fields because they
 * are indexed by number at the one place that uses them. Note the off-by-one
 * against the IDT: gate field 1 selects ist[0]. */
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed)) tss_t;

_Static_assert(sizeof(tss_t) == 104, "the long-mode TSS is 104 bytes");
_Static_assert(offsetof(tss_t, rsp0) == 4, "rsp0 sits at offset 4");
_Static_assert(offsetof(tss_t, ist) == 36, "IST1 sits at offset 36");
_Static_assert(offsetof(tss_t, iomap_base) == 102, "iomap_base sits at offset 102");

/* 4 KiB is generous for what runs on it: one exception frame and
 * exception_dispatch, whose locals are a handful of integers. It is a whole
 * page because the fault it exists for is a stack fault, and the point is to
 * be somewhere completely unrelated to whatever overflowed. */
#define DOUBLE_FAULT_STACK_SIZE 4096

/* Both static, for two reasons that happen to coincide.
 *
 * Lifetime: the CPU reads the TSS through the descriptor whenever it delivers
 * an IST interrupt, which is long after tss_init() has returned. A local would
 * leave the task register pointing into a dead stack frame.
 *
 * Zeroing: the kernel has no memset, and every reserved field of a TSS must be
 * zero. Being static puts both objects in .bss, which the ELF loader
 * zero-fills - so the only fields assigned below are the ones that need a
 * value, and the rest are correct by construction.
 *
 * aligned(16) on the stack because the CPU aligns RSP to 16 bytes when it
 * switches to an IST stack; starting the region aligned means the top is too,
 * and nothing is wasted. */
static tss_t   tss;
static uint8_t double_fault_stack[DOUBLE_FAULT_STACK_SIZE] __attribute__((aligned(16)));

void tss_init(void)
{
    /* Stacks grow downwards, so the pointer handed to the CPU is the end of
     * the array, not its start. Getting this backwards would give the double
     * fault handler a stack that grows immediately out of its own buffer -
     * which is precisely the class of bug this stack exists to survive. */
    tss.ist[TSS_IST_DOUBLE_FAULT - 1] =
        (uint64_t)(uintptr_t)(double_fault_stack + DOUBLE_FAULT_STACK_SIZE);

    /* RSP0 stays zero. It is only consulted on an interrupt that raises the
     * privilege level, and nothing runs in ring 3 yet; the day something does,
     * this is the line that has to change first. */

    /* An I/O permission bitmap may follow the TSS, and iomap_base is its
     * offset from the start. Setting it to the size of the structure says the
     * bitmap begins exactly where the segment ends - which, since the
     * descriptor's limit stops there too, is how you say "there is no bitmap".
     * Leaving it zero would instead point the CPU at the start of the TSS and
     * have it read the reserved fields as I/O permissions. */
    tss.iomap_base = (uint16_t)sizeof(tss_t);

    /* The descriptor goes in the GDT, whose table is private to gdt.c - hence
     * asking it to do the write rather than reaching into it from here. Safe
     * to do after gdt_init() has already run lgdt: the CPU re-reads the table
     * from memory on each selector load, and the GDTR limit set there already
     * spans the two rows this occupies. */
    gdt_set_tss((uint64_t)(uintptr_t)&tss, (uint32_t)(sizeof(tss_t) - 1));

    /* ltr loads the task register from a GDT selector. Unlike the CS reload in
     * gdt_flush.asm this is expressible as inline asm - there is a real
     * instruction taking a plain operand - so it does not need its own file. */
    uint16_t selector = GDT_SEL_TSS;
    __asm__ __volatile__("ltr %0" : : "rm"(selector));

    serial_printf("[tss]   TSS at %x, %u bytes, selector=%x\n",
                  (uint64_t)(uintptr_t)&tss, (uint64_t)sizeof(tss_t),
                  (uint64_t)selector);
    serial_printf("[tss]   IST%u stack top=%x (%u bytes)\n",
                  (uint64_t)TSS_IST_DOUBLE_FAULT,
                  tss.ist[TSS_IST_DOUBLE_FAULT - 1],
                  (uint64_t)DOUBLE_FAULT_STACK_SIZE);
}
