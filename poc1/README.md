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

On Windows, mingw binutils cannot emit ELF, so use Clang and LLD for both
halves instead. From the MSYS2 UCRT64 shell:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-nasm \
                   mingw-w64-ucrt-x86_64-qemu make
cd poc1
make TOOLCHAIN=clang run-fat
```

`run-fat` is the one to start with on Windows: it lets QEMU present `build/esp`
as a FAT volume, so `parted` and `mtools` are not needed. `make TOOLCHAIN=clang`
builds the real GPT disk image once those two are available.

The Makefile probes the usual OVMF locations on Linux, macOS and MSYS2. If none
of them match your install, point it at the firmware yourself:

```sh
make TOOLCHAIN=clang run-fat \
     OVMF_CODE=/path/to/OVMF_CODE.fd OVMF_VARS=/path/to/OVMF_VARS.fd
```

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

| # | Goal                       | Criterion                                        |
| - | -------------------------- | ------------------------------------------------ |
| 1 | our code runs              | identifying string within two seconds of power-on |
| 2 | framebuffer access         | `fb_size` ≈ `stride × height × 4`                |
| 3 | memory map read            | reported total within 5% of the machine's RAM     |
| 4 | ELF kernel loaded          | every PT_LOAD placed, `e_entry` inside a segment  |
| 5 | left boot services         | `EFI_SUCCESS` within three map refreshes          |
| 6 | kernel draws               | pattern correct: no skew, no colour swap          |
| 7 | stable                     | five minutes with no reset and no triple fault    |
| 8 | consistent                 | ten runs out of ten (`make check`)               |

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

Both halves compile clean under `-Wall -Wextra` and the `BOOTX64.EFI` link
produces a valid PE32+ EFI application (subsystem 10, entry `efi_main`). The
end-to-end run in QEMU has **not** been executed yet — the results table in
section 3.4 of the research document is still empty and gets filled in from a
real `make run`.

## What comes after

In order: our own GDT and IDT, the physical memory manager over the memory map
we already collect, then the Local APIC timer and its calibration. The third one
is the point where we can start measuring the metric the whole project is judged
on — timing consistency.
