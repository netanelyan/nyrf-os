# nyrf OS - POC 1 build.
#
#   make            build BOOTX64.EFI, kernel.elf and the GPT disk image
#   make run        boot the image in QEMU with OVMF and a serial log
#   make run-fat    same, but with QEMU's virtual FAT (no parted/mtools needed)
#   make debug      boot halted with a GDB stub on :1234
#   make check      ten headless runs, the consistency test of POC goal 8
#   make clean      remove build/
#
# Toolchains:
#   make                    GNU: mingw-w64 GCC for the EFI side, GCC/ld for ELF
#   make TOOLCHAIN=clang    Clang/LLD for both, the only option on Windows,
#                           because mingw binutils cannot emit ELF

TOOLCHAIN ?= gcc
BUILD     := build
ESP       := $(BUILD)/esp

NASM  ?= nasm
QEMU  ?= qemu-system-x86_64

ifeq ($(TOOLCHAIN),clang)
  EFI_CC     := clang --target=x86_64-unknown-windows
  EFI_LINK    = $(EFI_CC) -nostdlib -fuse-ld=lld \
                -Wl,-subsystem:efi_application -Wl,-entry:efi_main
  KERNEL_CC  := clang --target=x86_64-unknown-none-elf
  KERNEL_LD  := ld.lld
else
  EFI_CC     := x86_64-w64-mingw32-gcc
  EFI_LINK    = $(EFI_CC) -nostdlib \
                -Wl,--subsystem,10 -Wl,--entry=efi_main -Wl,--no-insert-timestamp
  KERNEL_CC  := gcc
  KERNEL_LD  := ld
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
KERNEL_CFLAGS := $(COMMON_CFLAGS) -fno-pic -fno-pie -mgeneral-regs-only
KERNEL_LDFLAGS := -nostdlib -static -no-pie -z max-page-size=0x1000 -T kernel/link.ld

BOOT_SRC   := boot/main.c boot/elf.c boot/serial.c
BOOT_OBJ   := $(BOOT_SRC:%.c=$(BUILD)/%.efi.o)
KERNEL_SRC := kernel/main.c boot/serial.c
KERNEL_OBJ := $(BUILD)/kernel/entry.o $(KERNEL_SRC:%.c=$(BUILD)/%.k.o)

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

.PHONY: all run run-fat debug check clean
.DELETE_ON_ERROR:

all: $(IMAGE)

# -- bootloader: PE32+, EFI application subsystem ---------------------------

$(BUILD)/boot/%.efi.o: boot/%.c
	@mkdir -p $(dir $@)
	$(EFI_CC) $(EFI_CFLAGS) -c $< -o $@

$(EFI_APP): $(BOOT_OBJ)
	@mkdir -p $(dir $@)
	$(EFI_LINK) $^ -o $@

# -- kernel: ELF64, fixed physical address ----------------------------------

$(BUILD)/kernel/entry.o: kernel/entry.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

$(BUILD)/%.k.o: %.c
	@mkdir -p $(dir $@)
	$(KERNEL_CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL): $(KERNEL_OBJ) kernel/link.ld
	@mkdir -p $(dir $@)
	$(KERNEL_LD) $(KERNEL_LDFLAGS) $(KERNEL_OBJ) -o $@

# -- ESP layout, shared by both image targets -------------------------------

.PHONY: esp
esp: $(EFI_APP) $(KERNEL)
	@mkdir -p $(ESP)/EFI/BOOT
	cp $(EFI_APP) $(ESP)/EFI/BOOT/BOOTX64.EFI
	cp $(KERNEL) $(ESP)/kernel.elf

# -- GPT disk image with a FAT32 ESP, built without root ---------------------

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
	$(QEMU) $(QEMU_FLAGS) -drive format=raw,file=fat:rw:$(ESP) -serial stdio

debug: $(IMAGE) $(BUILD)/OVMF_VARS.fd
	@echo "connect with: gdb $(KERNEL) -ex 'target remote :1234'"
	$(QEMU) $(QEMU_FLAGS) -drive format=raw,file=$(IMAGE) -serial stdio -s -S

# POC goal 8: ten consecutive runs, ten successes. A single failure means the
# code depends on timing - most likely in the memory map refresh loop.
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
