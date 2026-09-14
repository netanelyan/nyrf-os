/* Global Descriptor Table - construction and loading. See kernel/gdt.h for
 * why a 64-bit kernel still needs one at all. */

#include <kernel/gdt.h>
#include <kernel/serial.h>

/* One GDT entry, 8 bytes.
 *
 * The field order looks deranged because it is: base and limit were widened
 * twice, in the 286 to 386 transition and again for long mode, and each time
 * the new bits were bolted onto the end rather than moved. Read it as a
 * historical artefact, not a design.
 *
 *   bits  0-15   limit  0-15
 *   bits 16-31   base   0-15
 *   bits 32-39   base  16-23
 *   bits 40-47   access byte
 *   bits 48-51   limit 16-19
 *   bits 52-55   flags
 *   bits 56-63   base  24-31
 */
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  limit_high_flags; /* low nibble limit 16-19, high nibble flags */
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

_Static_assert(sizeof(gdt_entry_t) == 8, "a GDT entry must be exactly 8 bytes");

/* What lgdt actually reads: a 2-byte limit followed by an 8-byte base.
 * The limit is size - 1, the offset of the last valid byte, not the size. */
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr_t;

_Static_assert(sizeof(gdt_ptr_t) == 10, "the GDTR image must be 10 bytes");

/* A system descriptor - the TSS is the only one this kernel uses - is twice
 * the width of a code or data entry. Long mode needed a 64-bit base and there
 * was nowhere left in the 8-byte layout to put the top half, so the descriptor
 * was extended into a second row: the first eight bytes are the classic entry
 * above, and the second eight carry base bits 32-63 and padding.
 *
 * Only the first row's index is ever loaded as a selector. The second is not a
 * descriptor at all, and the CPU faults if a selector names it. */
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  limit_high_flags;
    uint8_t  base_high;
    uint32_t base_upper; /* base 32-63 */
    uint32_t reserved;
} __attribute__((packed)) tss_descriptor_t;

_Static_assert(sizeof(tss_descriptor_t) == 16, "a long-mode TSS descriptor is 16 bytes");
_Static_assert(sizeof(tss_descriptor_t) == 2 * sizeof(gdt_entry_t),
               "the TSS descriptor must occupy exactly two GDT rows");

/* -- access byte -----------------------------------------------------------
 *
 *   bit 7  P    present
 *   bit 6-5     DPL, the ring this segment belongs to
 *   bit 4  S    1 = code or data, 0 = a system descriptor such as the TSS
 *   bit 3  E    executable
 *   bit 2  DC   direction (data) / conforming (code)
 *   bit 1  RW   readable (code) / writable (data)
 *   bit 0  A    accessed, the CPU sets this itself
 */
#define ACC_PRESENT   0x80
#define ACC_DPL0      0x00
#define ACC_SEGMENT   0x10 /* S: a normal code/data segment */
#define ACC_EXEC      0x08
#define ACC_RW        0x02

/* With S clear, bits 3-0 stop being independent flags and become a system
 * descriptor type instead. 0x9 is "64-bit TSS, available"; 0xB is the same TSS
 * once ltr has loaded it, a change the CPU makes itself. */
#define ACC_TSS_AVAIL 0x09

#define ACCESS_KERNEL_CODE (ACC_PRESENT | ACC_DPL0 | ACC_SEGMENT | ACC_EXEC | ACC_RW) /* 0x9A */
#define ACCESS_KERNEL_DATA (ACC_PRESENT | ACC_DPL0 | ACC_SEGMENT | ACC_RW)            /* 0x92 */
/* No ACC_SEGMENT here: that is the bit saying "system descriptor". */
#define ACCESS_TSS         (ACC_PRESENT | ACC_DPL0 | ACC_TSS_AVAIL)                   /* 0x89 */

/* -- flags nibble ----------------------------------------------------------
 *
 *   bit 3  G    granularity: limit counts 4 KiB pages rather than bytes
 *   bit 2  DB   default operand size, 1 = 32-bit
 *   bit 1  L    64-bit code segment
 *   bit 0  AVL  free for software use
 *
 * L and DB must never both be set. A 64-bit code segment has L=1, DB=0.
 */
#define FLAG_GRANULARITY 0x8
#define FLAG_DB          0x4
#define FLAG_LONG        0x2

#define FLAGS_KERNEL_CODE (FLAG_GRANULARITY | FLAG_LONG) /* 0xA */
#define FLAGS_KERNEL_DATA (FLAG_GRANULARITY | FLAG_DB)   /* 0xC */

/* Five rows: null, kernel code, kernel data, and two taken by the single TSS
 * descriptor. Raising this is what makes room for the TSS - the GDTR limit is
 * computed from sizeof(gdt), so entries 3 and 4 are inside the table the CPU
 * will read even though gdt_init() itself leaves them zero. */
#define GDT_ENTRIES 5

/* Both must outlive gdt_init(): the CPU keeps reading the table, and GDTR
 * holds a pointer to it. A local would be on a stack frame that is gone. */
static gdt_entry_t gdt[GDT_ENTRIES];
static gdt_ptr_t   gdtr;

/* kernel/arch/x86_64/gdt_flush.asm */
extern void gdt_flush(const gdt_ptr_t *ptr, uint16_t code_sel, uint16_t data_sel);

/* Base and limit are ignored for code and data in long mode. The classic
 * full-range values go in anyway: they cost nothing, they match every
 * reference you will compare this against, and they matter if a 32-bit
 * compatibility segment is ever added. */
static void gdt_set(int index, uint8_t access, uint8_t flags)
{
    gdt[index].limit_low        = 0xFFFF;
    gdt[index].base_low         = 0;
    gdt[index].base_mid         = 0;
    gdt[index].access           = access;
    gdt[index].limit_high_flags = (uint8_t)(0x0F | (flags << 4));
    gdt[index].base_high        = 0;
}

/* See kernel/gdt.h. Called by tss_init() once it knows where its TSS lives.
 *
 * The cast writes one 16-byte descriptor across two 8-byte rows. It is the
 * kind of aliasing that would be undefined in portable C, and is sound here
 * for two specific reasons: the build passes -fno-strict-aliasing, and both
 * structures are packed, so neither has padding the other could disagree
 * about. Writing the two rows as raw 64-bit halves instead would be
 * standards-clean but would put the bit-shuffling back in the caller's face,
 * which is what the struct exists to avoid. */
void gdt_set_tss(uint64_t base, uint32_t limit)
{
    tss_descriptor_t *descriptor = (tss_descriptor_t *)&gdt[GDT_INDEX_TSS];

    descriptor->limit_low = (uint16_t)(limit & 0xFFFF);
    descriptor->base_low  = (uint16_t)(base & 0xFFFF);
    descriptor->base_mid  = (uint8_t)((base >> 16) & 0xFF);
    descriptor->access    = ACCESS_TSS;
    /* Flags nibble left zero: byte granularity, because a TSS is 104 bytes and
     * a page-granular limit could not describe it. */
    descriptor->limit_high_flags = (uint8_t)((limit >> 16) & 0x0F);
    descriptor->base_high        = (uint8_t)((base >> 24) & 0xFF);
    descriptor->base_upper       = (uint32_t)(base >> 32);
    descriptor->reserved         = 0;
}

void gdt_init(void)
{
    /* Entry 0 is the null descriptor and must be all zero bytes. Loading a
     * selector of 0 into a data register is legal and means "no segment";
     * using it for CS or SS faults. It exists to catch an uninitialised
     * selector instead of letting it quietly address something. */
    gdt[0] = (gdt_entry_t){0};

    gdt_set(1, ACCESS_KERNEL_CODE, FLAGS_KERNEL_CODE);
    gdt_set(2, ACCESS_KERNEL_DATA, FLAGS_KERNEL_DATA);

    gdtr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdtr.base  = (uint64_t)(uintptr_t)gdt;

    gdt_flush(&gdtr, GDT_SEL_KERNEL_CODE, GDT_SEL_KERNEL_DATA);

    /* Read CS back. If the far return in gdt_flush had not worked we would
     * not be here at all, but printing it turns "probably fine" into
     * evidence, and it is one instruction. */
    uint16_t cs = 0, ss = 0;
    __asm__ __volatile__("mov %%cs, %0" : "=r"(cs));
    __asm__ __volatile__("mov %%ss, %0" : "=r"(ss));

    serial_printf("[gdt]   table at %x, %u entries, limit=%u\n",
                  gdtr.base, (uint64_t)GDT_ENTRIES, (uint64_t)gdtr.limit);
    serial_printf("[gdt]   reloaded: CS=%x SS=%x\n", (uint64_t)cs, (uint64_t)ss);
}
