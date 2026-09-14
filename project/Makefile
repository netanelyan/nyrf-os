# nyrf OS build.
#
#   make            build BOOTX64.EFI, kernel.elf and the GPT disk image
#   make run        boot the image in QEMU with OVMF and a serial log
#   make run-fat    same, but with QEMU's virtual FAT (no parted/mtools needed)
#   make debug      boot halted with a GDB stub on :1234
#   make check      ten headless runs, the boot consistency test
#   make clean      remove build/
#
# Toolchains:
#   make                    GNU: mingw-w64 GCC for the EFI side, GCC/ld for ELF
#   make TOOLCHAIN=clang    Clang/LLD for both
#   make TOOLCHAIN=zig      Clang compiles, Zig links the kernel. This is the
#                           one that works on Windows: mingw binutils cannot
#                           emit ELF at all, and the LLD that MSYS2 ships is
#                           built COFF-only, so neither can link kernel.elf.
#                           Zig bundles an LLD with the ELF backend included.

# Three flavours of assignment appear below, and the difference matters:
#   ?=  set only if not already set, so the environment or the command line
#       wins - this is what makes `make TOOLCHAIN=zig` work
#   :=  evaluated once, here and now
#   =   evaluated every time it is used, which is why the *_LINK variables
#       below can be defined before the tools they name are chosen
TOOLCHAIN ?= gcc
BUILD     := build
ESP       := $(BUILD)/esp

NASM  ?= nasm
QEMU  ?= qemu-system-x86_64
ZIG   ?= zig

CLANG_EFI  := clang --target=x86_64-unknown-windows
CLANG_KERN := clang --target=x86_64-unknown-none-elf

ifeq ($(TOOLCHAIN),clang)
  EFI_CC      := $(CLANG_EFI)
  EFI_LINK     = $(EFI_CC) -nostdlib -fuse-ld=lld \
                 -Wl,-subsystem:efi_application -Wl,-entry:efi_main
  KERNEL_CC   := $(CLANG_KERN)
  KERNEL_LINK  = ld.lld -nostdlib -static -no-pie \
                 -z max-page-size=0x1000 -T kernel/link.ld
else ifeq ($(TOOLCHAIN),zig)
  EFI_CC      := $(CLANG_EFI)
  EFI_LINK     = $(EFI_CC) -nostdlib -fuse-ld=lld \
                 -Wl,-subsystem:efi_application -Wl,-entry:efi_main
  KERNEL_CC   := $(CLANG_KERN)
  # zig cc drives its own bundled LLD, so the linker script and -z go through
  # -Wl, rather than being passed to the linker directly. The cache directory
  # is pinned into build/ because zig cannot always work out where AppData is
  # when it is launched from an MSYS2 make.
  KERNEL_LINK  = ZIG_GLOBAL_CACHE_DIR=$(BUILD)/.zig-cache \
                 ZIG_LOCAL_CACHE_DIR=$(BUILD)/.zig-cache \
                 $(ZIG) cc -target x86_64-freestanding-none -nostdlib -static \
                 -no-pie -Wl,-T,kernel/link.ld -Wl,-z,max-page-size=0x1000
else
  EFI_CC      := x86_64-w64-mingw32-gcc
  EFI_LINK     = $(EFI_CC) -nostdlib \
                 -Wl,--subsystem,10 -Wl,--entry=efi_main -Wl,--no-insert-timestamp
  KERNEL_CC   := gcc
  KERNEL_LINK  = ld -nostdlib -static -no-pie \
                 -z max-page-size=0x1000 -T kernel/link.ld
endif

# Freestanding in both halves: no libc, no stack protector, and no red zone,
# because an interrupt would silently corrupt it.
COMMON_CFLAGS := -std=c11 -Wall -Wextra -O2 -ffreestanding \
                 -fno-stack-protector -fno-stack-check -mno-red-zone \
                 -fno-strict-aliasing -Iinclude

# -fshort-wchar keeps L"" literals 16 bit, which is what UEFI expects.
EFI_CFLAGS := $(COMMON_CFLAGS) -fshort-wchar

# No SSE in the kernel: the POC never enables it, and letting GCC vectorise the
# drawing loop would fault on the first xmm instruction.
# KERNEL_CFLAGS_EXTRA is a hook for one-off defines from the command line,
# without editing this file:
#   make run-fat KERNEL_CFLAGS_EXTRA=-DNYRF_TEST_FAULT=2
KERNEL_CFLAGS := $(COMMON_CFLAGS) -fno-pic -fno-pie -mgeneral-regs-only \
                 $(KERNEL_CFLAGS_EXTRA)

# Globbed, so a new .c or .asm anywhere in the tree needs no edit here.
#
# serial.c lives under kernel/dev but is compiled into both halves: it is the
# only output channel that survives ExitBootServices, so the bootloader needs
# it too. Two compilations, two ABIs, two object suffixes.
#
# The $(SRC:%.c=$(BUILD)/%.o) form is a substitution reference: it rewrites
# every name in the list, so boot/main.c becomes build/boot/main.efi.o. The
# object tree therefore mirrors the source tree under build/, which is why the
# pattern rules below have to mkdir before compiling.
BOOT_SRC   := $(wildcard boot/*.c) kernel/dev/serial.c
BOOT_OBJ   := $(BOOT_SRC:%.c=$(BUILD)/%.efi.o)

KERNEL_SRC := $(wildcard kernel/*.c kernel/*/*.c kernel/*/*/*.c)
KERNEL_ASM := $(wildcard kernel/*.asm kernel/*/*.asm kernel/*/*/*.asm)
KERNEL_OBJ := $(KERNEL_ASM:%.asm=$(BUILD)/%.asm.o) \
              $(KERNEL_SRC:%.c=$(BUILD)/%.k.o)

EFI_APP := $(BUILD)/BOOTX64.EFI
KERNEL  := $(BUILD)/kernel.elf
IMAGE   := $(BUILD)/nyrf-os.img

# OVMF ships under a different path on every distribution.
OVMF_CANDIDATES := /usr/share/OVMF/OVMF_CODE.fd \
                   /usr/share/edk2/ovmf/OVMF_CODE.fd \
                   /usr/share/edk2-ovmf/x64/OVMF_CODE.fd \
                   /usr/share/qemu/edk2-x86_64-code.fd \
                   /opt/homebrew/share/qemu/edk2-x86_64-code.fd \
                   /ucrt64/share/qemu/edk2-x86_64-code.fd \
                   /mingw64/share/qemu/edk2-x86_64-code.fd
OVMF_CODE ?= $(firstword $(wildcard $(OVMF_CANDIDATES)))
OVMF_VARS_CANDIDATES := /usr/share/OVMF/OVMF_VARS.fd \
                        /usr/share/edk2/ovmf/OVMF_VARS.fd \
                        /usr/share/edk2-ovmf/x64/OVMF_VARS.fd \
                        /usr/share/qemu/edk2-i386-vars.fd \
                        /ucrt64/share/qemu/edk2-i386-vars.fd \
                        /mingw64/share/qemu/edk2-i386-vars.fd
OVMF_VARS ?= $(firstword $(wildcard $(OVMF_VARS_CANDIDATES)))

QEMU_FLAGS := -machine q35 -m 512M -cpu qemu64 -net none \
              -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
              -drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd

# QEMU on Windows cannot enumerate a vvfat directory whose path contains
# non-ASCII characters. It does not report an error either - it just presents
# an empty volume, and the firmware drops to the UEFI shell. cygpath -ws gives
# the 8.3 short form of the path, which is always plain ASCII. Off Windows
# there is no cygpath and the path is used unchanged.
ESP_QEMU := $(or $(shell cygpath -w -s "$(CURDIR)/$(ESP)" 2>/dev/null),$(ESP))

# .PHONY marks targets that are actions rather than files, so make runs them
# even if a file of that name ever appears in the directory.
#
# .DELETE_ON_ERROR removes a target whose recipe failed partway. Without it a
# half-written disk image would be left behind looking newer than its sources,
# and the next make would consider it up to date and boot it.
.PHONY: all run run-fat debug check clean
.DELETE_ON_ERROR:

all: $(IMAGE)

# -- bootloader: PE32+, EFI application subsystem ---------------------------
#
# The automatic variables in the recipes below are make's shorthand:
#   $@  the target being built
#   $<  the first prerequisite (the .c file, in a pattern rule)
#   $^  all prerequisites (every .o, in the link rules)
# $(dir $@) is the target's directory, created first because make will not
# create it and the object tree mirrors the source tree.
#
# A PE32+ binary with the EFI application subsystem is what the firmware will
# load; that is the entire difference between this and an ordinary executable.

$(BUILD)/%.efi.o: %.c
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(EFI_APP): $(BOOT_OBJ)
	@mkdir -p $(dir $@)
	$(EFI_LINK) $^ -o $@

# -- kernel: ELF64, fixed physical address ----------------------------------
#
# Note the kernel link rule depends on kernel/link.ld as well as the objects,
# so editing the layout relinks. The .asm rule runs NASM with -f elf64 to match
# the C objects; a mismatch there fails at link time rather than at boot.

$(BUILD)/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

$(BUILD)/%.k.o: %.c
	@mkdir -p $(dir $@)
	$(KERNEL_CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL): $(KERNEL_OBJ) kernel/link.ld
	@mkdir -p $(dir $@)
	$(KERNEL_LINK) $(KERNEL_OBJ) -o $@

# -- ESP layout, shared by both image targets -------------------------------

.PHONY: esp
esp: $(EFI_APP) $(KERNEL)
	@mkdir -p $(ESP)/EFI/BOOT
	cp $(EFI_APP) $(ESP)/EFI/BOOT/BOOTX64.EFI
	cp $(KERNEL) $(ESP)/kernel.elf

# -- GPT disk image with a FAT32 ESP, built without root ---------------------
#
# "Without root" is the point: the usual way to build a bootable image is to
# loop-mount a partition, which needs privileges. Instead the partition is
# built as a standalone file with mtools (mformat/mmd/mcopy write FAT directly,
# no mount involved), then dd'd into the right offset of the disk image.
#
# The sizes line up deliberately: a 64 MiB disk, a 63 MiB partition, written at
# a 1 MiB seek - matching the 1MiB start that parted was told to use. Change
# one and the other two have to follow.
#
# The firmware finds the result by convention alone: a GPT disk, a partition
# flagged esp, a FAT filesystem, and an executable at the hardcoded fallback
# path /EFI/BOOT/BOOTX64.EFI. No boot entry or NVRAM variable is involved,
# which is exactly why the image is portable to real hardware.

$(IMAGE): esp
	dd if=/dev/zero of=$(IMAGE) bs=1M count=64 status=none
	parted -s $(IMAGE) mklabel gpt
	parted -s $(IMAGE) mkpart ESP fat32 1MiB 100%
	parted -s $(IMAGE) set 1 esp on
	dd if=/dev/zero of=$(BUILD)/part.img bs=1M count=63 status=none
	mformat -i $(BUILD)/part.img -F -v NYRFOS ::
	mmd -i $(BUILD)/part.img ::/EFI ::/EFI/BOOT
	mcopy -i $(BUILD)/part.img $(EFI_APP) ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $(BUILD)/part.img $(KERNEL) ::/kernel.elf
	dd if=$(BUILD)/part.img of=$(IMAGE) bs=1M seek=1 conv=notrunc status=none
	@echo "built $(IMAGE)"

$(BUILD)/OVMF_VARS.fd:
	@mkdir -p $(BUILD)
	@test -n "$(OVMF_VARS)" || { echo "OVMF_VARS.fd not found; set OVMF_VARS=..."; exit 1; }
	cp $(OVMF_VARS) $@

# -- running ----------------------------------------------------------------

run: $(IMAGE) $(BUILD)/OVMF_VARS.fd
	$(QEMU) $(QEMU_FLAGS) -drive format=raw,file=$(IMAGE) -serial stdio

# Skips parted and mtools by letting QEMU present build/esp as a FAT volume.
run-fat: esp $(BUILD)/OVMF_VARS.fd
	$(QEMU) $(QEMU_FLAGS) -drive 'format=raw,file=fat:rw:$(ESP_QEMU)' -serial stdio

debug: $(IMAGE) $(BUILD)/OVMF_VARS.fd
	@echo "connect with: gdb $(KERNEL) -ex 'target remote :1234'"
	$(QEMU) $(QEMU_FLAGS) -drive format=raw,file=$(IMAGE) -serial stdio -s -S

# Ten consecutive runs, ten successes. A single failure means the
# code depends on timing - most likely in the memory map refresh loop.
#
# This is the POC's consistency criterion, and the reason it is ten runs rather
# than one: a firmware race shows up as an occasional failure, and a single
# green run proves nothing about a system whose whole premise is deterministic
# timing.
#
# The recipe is one long shell command - hence the trailing backslashes, which
# keep it in a single shell so that $$pass survives across the loop. $$ is an
# escaped $, passed through to the shell rather than expanded by make.
#
# -display none makes it headless, and the serial log goes to a file per run.
# `|| true` swallows the timeout's exit status so one hung run does not abort
# the loop; the grep for the kernel's last line is the real verdict, and the
# final `test` is what sets the exit code make reports.
check: $(IMAGE) $(BUILD)/OVMF_VARS.fd
	@pass=0; \
	for i in 1 2 3 4 5 6 7 8 9 10; do \
	  log=$(BUILD)/run-$$i.log; \
	  timeout 30 $(QEMU) $(QEMU_FLAGS) -drive format=raw,file=$(IMAGE) \
	    -display none -serial file:$$log >/dev/null 2>&1 || true; \
	  if grep -q "test pattern drawn" $$log 2>/dev/null; then \
	    pass=$$((pass+1)); echo "run $$i: PASS"; \
	  else \
	    echo "run $$i: FAIL (see $$log)"; \
	  fi; \
	done; \
	echo "$$pass/10 passed"; \
	test $$pass -eq 10

clean:
	rm -rf $(BUILD)