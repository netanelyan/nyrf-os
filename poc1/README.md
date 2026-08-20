# nyrf OS — POC 1

The first proof of concept for [nyrf OS](../README.md), specified in
[`docs/research-and-poc.docx`](../docs/research-and-poc.docx) chapter 3.

Everything in this folder is self-contained: build and run it from here.

## What POC 1 does

Our own UEFI application comes up on a bare machine, gets the screen and the
memory map from the firmware, loads an external ELF kernel, leaves boot
services, and hands control to that kernel, which draws a test pattern straight
into the framebuffer.

It covers stages 0 to 5 of the boot sequence and stops there. No descriptor
tables, no paging, no memory manager, no interrupts, no scheduler — those all
run *inside* a kernel that has already booted, which is exactly what this POC
is trying to prove is possible.

The POC attacks the biggest unknown in the project rather than the biggest
piece of work. A memory manager and a scheduler are complex but predictable;
the UEFI handoff is where we depend on firmware behaviour we do not control. If
this fails, we want to know in week two, not in month six.

## Flow

```
power on
  -> UEFI firmware: POST, memory controller, PCIe, ACPI tables
  -> firmware loads \EFI\BOOT\BOOTX64.EFI from the FAT32 ESP
  -> efi_main: disable watchdog, init UART on 0x3F8
  -> LocateProtocol(GOP): framebuffer base, resolution, stride, pixel format
  -> GetMemoryMap + RSDP from the configuration table
  -> open \kernel.elf on the ESP, load every PT_LOAD segment
  -> GetMemoryMap again for a fresh MapKey
  -> ExitBootServices  (retry up to 3x on a stale key; no way back after this)
  -> jump to kernel entry with a boot_info_t pointer
  -> kernel_main: draw the test pattern, then hlt forever
```

Everything the firmware can tell us is collected *before* `ExitBootServices`
and passed over in one `boot_info_t` — afterwards there is no way to ask again.

## The test pattern

Three vertical bars, one per colour channel, each a dark-to-bright ramp, plus a
one pixel white frame around the screen.

Both details are deliberate. Swapped RGB/BGR shows up instantly as red and blue
trading places. Confusing `stride` with `width` makes the frame run diagonally
instead of square. A single filled rectangle would have caught neither.

## Layout

| Path                  | Contents                                            |
| --------------------- | --------------------------------------------------- |
| `boot/main.c`         | `efi_main` — all the logic against the firmware      |
| `boot/elf.c`          | ELF64 header parsing and PT_LOAD placement           |
| `boot/serial.c`       | 16550 UART logging on COM1, used by both halves      |
| `kernel/main.c`       | `kernel_main` — draws the test pattern               |
| `kernel/entry.asm`    | entry point, stack setup, jump into C                |
| `kernel/link.ld`      | kernel memory layout, fixed at 1 MiB physical        |
| `include/bootinfo.h`  | the one structure shared by both halves              |
| `include/uefi.h`      | the UEFI types and protocols we actually use         |
| `Makefile`            | both binaries, the disk image, and the QEMU targets  |

## Building

Requirements: an ELF-capable C toolchain, a PE32+ one, `nasm`, `make`, `qemu`
with `OVMF`, and `parted` + `mtools` for the disk image.

All commands run from this folder.

On Debian/Ubuntu:

```sh
sudo apt install build-essential gcc-mingw-w64-x86-64 nasm \
                 qemu-system-x86 ovmf parted mtools
cd poc1
make          # build/BOOTX64.EFI, build/kernel.elf, build/nyrf-os.img
make run      # boot it in QEMU with a serial log on stdout
```

On Windows, **neither** available linker can produce ELF: mingw binutils has no
ELF backend at all, and the LLD that MSYS2 ships is built COFF-only, so it
rejects `-T` and `-z` no matter which flavour you ask for. Zig bundles an LLD
that does include the ELF backend, so it links the kernel while Clang compiles
both halves — that is what `TOOLCHAIN=zig` selects. LLD is still needed for the
bootloader, whose link is COFF.

From the MSYS2 UCRT64 shell:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-lld \
                   mingw-w64-ucrt-x86_64-nasm mingw-w64-ucrt-x86_64-qemu make
```

Then Zig, which is a plain zip with no installer:

```sh
curl -LO https://ziglang.org/download/0.16.0/zig-x86_64-windows-0.16.0.zip
unzip -q zig-x86_64-windows-0.16.0.zip -d /opt
mv /opt/zig-x86_64-windows-0.16.0 /opt/zig
echo 'export PATH="/opt/zig:$PATH"' >> ~/.bashrc && export PATH="/opt/zig:$PATH"
```

```sh
cd poc1
make TOOLCHAIN=zig run-fat
```

`run-fat` is the one to start with on Windows: it lets QEMU present `build/esp`
as a FAT volume, so `parted` and `mtools` are not needed. Plain `make` builds
the real GPT disk image once those two are available.

The Makefile probes the usual OVMF locations on Linux, macOS and MSYS2. If none
of them match your install, point it at the firmware yourself:

```sh
make TOOLCHAIN=zig run-fat \
     OVMF_CODE=/path/to/OVMF_CODE.fd OVMF_VARS=/path/to/OVMF_VARS.fd
```

### Non-ASCII paths on Windows

QEMU cannot enumerate a vvfat directory whose path contains non-ASCII
characters, and it does not report an error — it presents an *empty* volume, so
the firmware finds no `\EFI\BOOT\BOOTX64.EFI` and drops to the UEFI shell. It
looks exactly like a broken bootloader.

The Makefile works around it by passing QEMU the 8.3 short form of the ESP path
(`cygpath -w -s`), which is always plain ASCII. Nothing to do by hand, but it is
worth knowing if you ever run QEMU directly.

Other targets:

| Target         | Purpose                                                     |
| -------------- | ----------------------------------------------------------- |
| `make run-fat` | boot via QEMU's virtual FAT, skipping `parted` and `mtools` |
| `make debug`   | boot halted with a GDB stub on `:1234`                      |
| `make check`   | ten headless runs — the consistency test of goal 8          |
| `make clean`   | remove `build/`                                             |

`make debug` pairs with `gdb build/kernel.elf -ex 'target remote :1234'`. If
the screen stays black, that is how you find out whether the kernel was ever
reached: `_start` is linked at exactly `0x100000`.

## Success criteria

From section 3.1 of the research document. The serial log prints one line per
stage, so the point of failure is visible immediately.

Measured on QEMU 9.x with OVMF, `-machine q35 -m 512M`, on 2026-08-20.

| # | Goal               | Criterion                                | Measured                                              | Pass |
| - | ------------------ | ---------------------------------------- | ----------------------------------------------------- | ---- |
| 1 | our code runs      | identifying string within 2 s             | string on serial and ConOut; full pattern by 2.5 s     | yes  |
| 2 | framebuffer access | `fb_size` ≈ `stride × height × 4`        | 1280×800, stride 1280, BGRX; 0x3E8000 = exactly 4096000 | yes  |
| 3 | memory map read    | total within 5% of the machine's RAM      | 106 descriptors, desc_size 48; 505 MiB usable of 512    | yes  |
| 4 | ELF kernel loaded  | every PT_LOAD placed, `e_entry` in one    | 3 of 4 headers are PT_LOAD, all placed; entry 0x100000  | yes  |
| 5 | left boot services | `EFI_SUCCESS` within 3 map refreshes      | `EFI_SUCCESS` on attempt 1, all ten runs                | yes  |
| 6 | kernel draws       | no skew, no colour swap                   | R/G/B left to right, frame square on all four edges     | yes  |
| 7 | stable             | 5 minutes, no reset, no triple fault      | one BDS boot, one draw, no reset in 5 min               | yes  |
| 8 | consistent         | ten runs out of ten                       | 10/10, 2395–2541 ms to pattern drawn                    | yes  |

Goal 6 was checked by sampling the captured framebuffer, not by eye: the corners
and both mid-row edges read `(255,255,255)`, and `(200,400)`, `(640,400)`,
`(1100,400)` read pure red, green and blue respectively. Red on the left is what
proves the BGRX byte order was handled correctly; a swap would put blue there.

The POC deliberately does **not** measure performance, jitter, frame rate or
memory use. Those are the project's metrics, not the POC's — measuring timing
needs a scheduler and a calibrated timer, both outside these boundaries.

## Note on POSIX-UEFI

The research document (section 1.4) chose POSIX-UEFI over the unmaintained
gnu-efi. This POC vendors `include/uefi.h` instead: the ~60 types and protocols
we actually touch, transcribed from the UEFI 2.10 spec.

The reason is that it keeps the repository self-contained — no download step in
the build, no second Makefile to reconcile with ours. The decision that mattered
in the research stands: we are not using gnu-efi. Swapping in POSIX-UEFI or
EDK2 later is a header change and nothing more.

## Status

**POC 1 passes.** All eight goals met on QEMU + OVMF; the table above is the
evidence for section 3.4 of the research document.

The serial log of a successful run, end to end:

```
[boot]  nyrf OS bootloader, POC 1
[boot]  ImageBase=0x000000001DD12000 size=0x0000000000005000
[boot]  GOP base=0x0000000080000000 size=0x00000000003E8000
[boot]  1280x800 stride=1280 format=BGRX
[boot]  memory map: 106 descriptors, desc_size=48
[boot]  mapped 13059 MiB, usable after boot services 505 MiB
[boot]  RSDP=0x000000001F77E014
[elf]   ELF64 x86_64 executable, entry=0x0000000000100000, 4 program headers
[elf]   PT_LOAD 0 -> 0x0000000000100000  filesz=0x8B1 memsz=0x8B1 (1 pages)
[elf]   PT_LOAD 1 -> 0x0000000000101000  filesz=0x161 memsz=0x161 (1 pages)
[elf]   PT_LOAD 2 -> 0x0000000000102000  filesz=0x0   memsz=0x4000 (4 pages)
[elf]   3 segments loaded
[boot]  kernel entry=0x0000000000100000
[boot]  calling ExitBootServices
[boot]  ExitBootServices OK on attempt 1
[boot]  jumping to kernel
[kern]  nyrf OS kernel reached
[kern]  fb=0x0000000080000000 1280x800 stride=1280
[kern]  test pattern drawn, halting
```

Two details in that log are worth keeping. The fourth program header is
`PT_GNU_STACK`, not `PT_LOAD`, and is correctly skipped — the loader must filter
on `p_type` rather than trusting `e_phnum`. And the third `PT_LOAD` has
`filesz=0, memsz=0x4000`: that is `.bss`, and it is the reason the loader has to
zero everything between `p_filesz` and `p_memsz`.

Still outstanding: the run on real hardware, which the research document
(section 3.2) deliberately placed outside the POC boundary because it adds
Secure Boot and per-vendor firmware differences.

## What comes after

In order: our own GDT and IDT, the physical memory manager over the memory map
we already collect, then the Local APIC timer and its calibration. The third one
is the point where we can start measuring the metric the whole project is judged
on — timing consistency.
