# `project/` — structure notes

These notes cover every file in `project/`, what it does, and how the pieces fit
together. `project/` is POC 1's code re-laid-out as a real OS source tree: same
boot flow as `poc1/`, but `serial` moved into `kernel/dev/`, headers moved into
`include/`, and the entry stub moved under `kernel/arch/x86_64/`. `poc1/` is
frozen as the proof snapshot; `project/` is the tree that grows from here.

## The one idea to hold on to

There are **two separate programs** in this directory, built by two different
compilers into two different formats, and they meet exactly once:

| | bootloader | kernel |
| --- | --- | --- |
| source | `boot/` | `kernel/` |
| format | PE32+ (`BOOTX64.EFI`) | ELF64 (`kernel.elf`) |
| ABI | Microsoft x64 (`ms_abi`) | System V (`sysv_abi`) |
| runs | under UEFI firmware | on bare metal, firmware gone |
| can call firmware? | yes, until `ExitBootServices` | no, never |

The handoff is a single call — `boot/main.c:366` jumps to the kernel entry with
one pointer in RDI. That pointer is a `boot_info_t`. **Everything** the kernel
will ever know about the machine has to be inside that struct, because after
`ExitBootServices` there is no firmware left to ask.

## Directory map

```
project/
├── Makefile                      both builds, the disk image, QEMU, the 10x test
├── include/                      headers shared across the two halves
│   ├── bootinfo.h                THE handoff contract (read this first)
│   ├── uefi.h                    vendored UEFI types/protocols we touch
│   └── kernel/                   kernel headers: serial.h (used by both
│                                 halves), gdt.h, tss.h, idt.h, pmm.h
├── boot/                         the UEFI application — runs first
│   ├── main.c                    efi_main, boot stages 0-5, the jump
│   ├── elf.c                     ELF64 parse + PT_LOAD placement
│   └── elf.h                     one function: elf_load_kernel()
└── kernel/                       the ELF kernel — runs after the jump
    ├── arch/x86_64/entry.asm     _start: stack setup, then call into C
    ├── arch/x86_64/gdt.c, tss.c  our own GDT and TSS (IST1 for #DF)
    ├── arch/x86_64/idt.c, isr.asm  exception stubs + register dump
    ├── main.c                    kernel_main: tables, memory, test pattern, halt
    ├── mm/pmm.c                  physical memory manager (bitmap, 4 KiB pages)
    ├── lib/string.c              memset/memcpy the compiler may emit calls to
    ├── dev/serial.c              16550 UART driver (compiled into BOTH halves)
    └── link.ld                   fixed load address 0x100000, section layout
```

## Read them in this order

1. `include/bootinfo.h` — 56 lines, the contract. Nothing else makes sense first.
2. `boot/main.c` — the boot sequence, top to bottom, in order.
3. `kernel/arch/x86_64/entry.asm` → `kernel/main.c` — the other side of the jump.
4. `Makefile` — why there are two of everything.

Leave `include/uefi.h` for last: it is 313 lines of transcribed spec, reference
material rather than logic.

---

## `include/bootinfo.h` — the handoff contract

A packed struct, 72 bytes, carrying:

- **framebuffer**: `fb_base`, `fb_size`, `width`, `height`, `stride`,
  `pixel_format`
- **memory map**: `mmap_ptr`, `mmap_size`, `desc_size` — collected now, used by
  the future physical memory manager
- **ACPI**: `rsdp` — collected now, used by the future APIC timer
- **`magic`** = `"NYRFOS01"`. The kernel refuses to draw if it does not match,
  so a wrong pointer becomes a clean stop instead of garbage on screen.

Two details worth internalising, because they are the reason this file is the
important one:

- **Compiled twice, two ABIs.** Only the *layout* has to agree, which is why
  every field is fixed-width and the struct is packed.
- **`_Static_assert` on `sizeof` and four offsets.** These fire in *both*
  compilations, so a future edit that moves a field breaks the build instead of
  producing a garbled screen. If you add a field, add it at the end and update
  the assertions deliberately.

`stride` vs `width` is the trap: `stride` (`PixelsPerScanLine`) includes
padding, `width` does not. Row arithmetic always uses `stride`.

## `boot/main.c` — the UEFI application

`efi_main` is the firmware's entry point; everything above it is support. The
sequence, with line refs:

| stage | what | lines |
| --- | --- | --- |
| 0 | disarm the 5-minute watchdog, `serial_init` | 263-267 |
| 2 | `locate_gop` + normalise the pixel format | 277-291 |
| 3 | `get_memory_map`, report totals | 294-299 |
| 4 | `find_rsdp` from the configuration table | 301 |
| 5 | open `\kernel.elf` on the ESP, load it | 305-313 |
| — | allocate + fill `boot_info_t` in its own page | 317-331 |
| 6 | refresh map → `ExitBootServices` (retry ×3) | 337-359 |
| 7 | jump to the kernel, never return | 366 |

Non-obvious points, all of them deliberate:

- **`memset`/`memcpy` are defined locally** (32-49). GCC lowers struct
  assignment into calls to these even under `-ffreestanding`, and there is no
  libc to link.
- **The memory-map buffer is oversized by 8 descriptors** (160). `AllocatePool`
  itself changes the map, so asking for the exact size is a race with yourself.
  The buffer is allocated once and reused.
- **`ExitBootServices` runs in a refresh-and-retry loop** (337-354). The map key
  goes stale whenever anything allocates. This loop is *required*, not
  defensive — and the `make check` comment names it as the first suspect if a
  run ever fails.
- **Descriptors are walked with `desc_size` stride, not `sizeof`** (180-181).
  The firmware is allowed to make descriptors bigger than the struct.
- **`fatal()`** logs to serial *and* ConOut, then halts forever. A corrupt
  `kernel.elf` is supposed to land here — that is the POC's recovery test.
- **After `ExitBootServices`, `ST` and `BS` are nulled** (362-363) so any
  accidental later use faults loudly instead of working by luck.
- The kernel-entry typedef is explicitly `__attribute__((sysv_abi))` (23) —
  that is what puts `boot_info` in RDI across the ABI boundary.

## `boot/elf.c` — the loader

Self-contained ELF64 reader; local `elf64_ehdr_t` / `elf64_phdr_t`, no
`elf.h` from a toolchain.

- `read_at` loops, because the firmware is allowed to return a short read (47).
- `header_is_valid` checks magic, 64-bit LE, `ET_EXEC`, `EM_X86_64`, and a sane
  program-header table — each with its own log line, so a rejection tells you
  *why*.
- **`ET_EXEC` is required**, not `ET_DYN`: the kernel must be fixed-address.
- For each `PT_LOAD`: `AllocatePages(AllocateAddress, …)` at **`p_paddr`** —
  honoured rather than `p_vaddr`, because the firmware still has everything
  identity-mapped (138-146).
- The gap between `p_filesz` and `p_memsz` is `.bss` and is **zeroed by the
  loader** (162-166). `link.ld` deliberately keeps `.bss` in the same segment so
  this path is the one that runs.
- Returns `e_entry` through `*entry`.

## `kernel/arch/x86_64/entry.asm` — `_start`

34 lines. `cli` (no IDT yet), `cld` (System V wants DF clear), point RSP at its
own 16 KiB `.bss` stack, zero RBP to terminate the frame chain, `call
kernel_main` with RDI untouched. Hangs on `hlt` if C ever returns.

Its own section `.text.entry` exists so `link.ld` can place it **first** —
`_start` then sits at exactly `0x100000`, making "is RIP even in the kernel?" a
single glance in GDB.

Why a separate stack at all: the firmware stack lives in memory the kernel is
free to reclaim.

## `kernel/main.c` — the whole POC kernel

`serial_init`, verify magic, then one nested loop drawing a **graded** test
pattern, then halt. The pattern is chosen to fail visibly:

- three vertical bars (R/G/B), each a dark→bright ramp → a swapped RGB/BGR
  order shows up as red and blue trading places, and banding is visible too
- a one-pixel white frame → if `stride` were confused with `width`, the frame
  would run diagonally instead of square

A single filled rectangle would have caught neither. `pack()` handles both byte
orders so the kernel never needs `uefi.h`.

Since the POC it also loads its own GDT, TSS and IDT and brings up the
physical memory manager before drawing. Still no paging of its own and no
scheduler.

## `kernel/mm/pmm.c` — physical memory manager

A bitmap, one bit per 4 KiB page, 1 = used, built from the UEFI memory map in
`boot_info_t`. 16 KiB of bitmap covers 512 MiB.

- **Sized by RAM, not by the map.** The map on QEMU runs to 13 GiB because of
  PCI windows; the bitmap covers only up to the highest conventional /
  boot-services / loader-code page.
- **Starts all-used, carves out free.** Only `EfiConventionalMemory` is marked
  free. Boot-services memory is covered but stays reserved: it holds the page
  tables the CPU is still using. Reclaiming it is part of the paging step.
- **Always reserved:** the first MiB (so page 0 is never handed out, `0` can
  mean "out of memory", and an SMP trampoline has somewhere to go later), the
  kernel image (`__kernel_start`..`__kernel_end` from `link.ld`), and the
  bitmap's own pages.
- **Allocation is next-fit over 64-bit words**, skipping full words in one
  compare and finding a free bit with `ctz`. Still a scan, so its cost depends
  on memory state — acceptable while nothing timing-critical allocates, and
  the thing to replace (free list / pools on top of the bitmap) when something
  does.
- **`pmm_free_page` halts** on an unaligned address, an address outside tracked
  memory, or a double free.
- `pmm_self_test` runs on every boot (allocate 3, write/read a tag, free,
  check the count), so `make check` covers the allocator too.

Physical addresses are usable as pointers only because the firmware's identity
map is still live. That stops being true at the paging step.

## `kernel/dev/serial.c` — 16550 UART on COM1

Polled, interrupt-free, `outb`/`inb` inline asm. `serial_init` sets divisor 1
(115200 baud), 8N1, FIFOs on. Plus `serial_putc/puts/put_hex/put_dec` and a
tiny `serial_printf` supporting `%s %x %u %c %%`.

**This file is compiled into both halves** — twice, two ABIs, two object
suffixes (`.efi.o` and `.k.o`). It is the only output channel that survives
`ExitBootServices`, so without it a black screen is indistinguishable from a
kernel that was never reached. Note `%x` and `%u` take `uint64_t`, hence all the
explicit casts at the call sites.

## `kernel/link.ld`

`ENTRY(_start)`, load address `KERNEL_PHYS_BASE = 0x100000` (1 MiB — everything
below is legacy/firmware territory). Sections in order: `.text` (with
`.text.entry` first), `.rodata`, `.data`, `.bss`, then `__kernel_end`.
`/DISCARD/` drops `.comment`, `.note*`, `.eh_frame*` — nothing needed at
runtime, and it keeps the `PT_LOAD` list short enough to read at a glance in
the log.

## `Makefile`

Targets: `all`, `run`, `run-fat`, `debug`, `check`, `clean`.

Two toolchain paths per half, selected by `TOOLCHAIN=gcc|clang|zig`:

- **`zig` is the one that works on Windows.** mingw binutils cannot emit ELF at
  all, and MSYS2's LLD is built COFF-only — so neither can link `kernel.elf`.
  Zig bundles an LLD with the ELF backend. (This is why the recent commits are
  about adding a zig toolchain.)
- Flags that matter: `-mno-red-zone` (an interrupt would silently corrupt it),
  `-fshort-wchar` for the EFI side (UEFI wants 16-bit `L""`), and
  `-mgeneral-regs-only` for the kernel — no SSE is ever enabled, so a
  vectorised drawing loop would fault on the first `xmm`.
- Sources are **globbed**, so a new `.c`/`.asm` anywhere in the tree needs no
  Makefile edit. `kernel/dev/serial.c` is the one file listed explicitly, in
  `BOOT_SRC`, because both halves need it.
- Image build uses `parted` + `mtools` to make a GPT disk with a FAT32 ESP
  **without root**. `run-fat` skips both tools by letting QEMU present
  `build/esp` as a FAT volume.
- `OVMF_CODE`/`OVMF_VARS` are probed across 7 known distro paths.
- `ESP_QEMU` runs the path through `cygpath -ws`: QEMU on Windows silently
  presents an *empty* volume for a vvfat path containing non-ASCII characters,
  and the firmware then drops to the UEFI shell. The 8.3 short form is always
  ASCII.
- **`make check` is the consistency test**: 10 headless runs, greps each log for
  `"test pattern drawn"`, requires 10/10. A single failure means the code
  depends on timing — most likely the memory-map refresh loop.

---

## Where the next work goes

Done: GDT/TSS/IDT, physical memory manager. Next, in order: our own page
tables (built from `pmm_alloc_page`, after which boot-services memory can be
reclaimed) → Local APIC timer. `kernel/dev/` is where the timer driver goes,
and `boot_info_t` already carries the RSDP it needs. Neither requires touching
the bootloader.
