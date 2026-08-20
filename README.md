# nyrf OS

An x86_64 operating system built from scratch — bootloader, kernel and every
layer underneath — booted through UEFI and written in C and assembly. Year
project, Magshimim.

The goal is to implement, from the ground up, the layers every modern operating
system rests on: from power-on (UEFI), through the kernel, memory management
and scheduling, down to input and output drivers — and to see how far overhead
can be cut to get a system that is lean, fast and consistent in its timing.

What makes the project technically distinct is that **timing consistency**, not
raw compute, is the requirement that drives every other design decision. A
general-purpose kernel that runs processes is not meaningfully different from
any other hobby OS; deterministic scheduling and direct I/O access being the
organising constraint is what makes this one its own thing.

## Repository layout

| Path                  | Contents                                              |
| --------------------- | ----------------------------------------------------- |
| [`poc1/`](poc1/)      | **POC 1** — UEFI boot, ELF kernel handoff, framebuffer |
| [`docs/`](docs/)      | project initiation and research/POC documents          |

## POC 1

The first proof of concept is complete and **passing** on QEMU + OVMF — all
eight success criteria met, 10 runs out of 10. It lives in [`poc1/`](poc1/),
with its own README covering the flow, the build and the measured results.

In one line: our own UEFI application comes up on a bare machine, takes the
screen and the memory map from the firmware, loads an external ELF kernel,
leaves boot services, and hands control to that kernel — which draws a test
pattern straight into the framebuffer.

```sh
cd poc1
make          # build BOOTX64.EFI, kernel.elf and the bootable disk image
make run      # boot it in QEMU with OVMF and a serial log on stdout
```

It attacks the riskiest part of the project rather than the largest one. A
memory manager and a scheduler are complex but predictable; the UEFI handoff is
where we depend on firmware behaviour we do not control. If it fails, we want
to know in week two, not in month six.

## Roadmap

POC 1 stops at a kernel that has booted and drawn one frame. In order after it:

1. our own GDT and IDT — the precondition for handling any interrupt
2. a physical memory manager over the memory map POC 1 already collects
3. the Local APIC timer and its calibration

The third brings us to the point where we can start measuring the metric the
whole project is judged on: timing consistency.

Beyond that: a framebuffer graphics driver, a USB HID input path for keyboard
and gamepad, and the deterministic scheduler itself.
