# lnxrm -- x86-64 Unix-like kernel (asm + C + C++ + Rust)
# Deliverable: build/bzImage  ->  qemu-system-x86_64 -kernel build/bzImage

CROSS   :=
CC      := $(CROSS)gcc
CXX     := $(CROSS)g++
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
RUSTC   := rustc
NASM    := nasm
PYTHON3 := python3

BUILD   := build
ARCHDIR := arch

WARN    := -Wall -Wno-unused-function -Wno-unused-variable -Wno-format
KERNFLAGS := -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
             -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel \
             -O2 -g ${WARN} -std=gnu11 -Iinclude -Ilib -I. -MMD -MP
CXXFLAGS  := $(KERNFLAGS) -std=c++20 -fno-exceptions -fno-rtti \
             -fno-threadsafe-statics -fno-use-cxa-atexit -nostdinc++

RUSTFLAGS := --crate-type staticlib --edition 2021 -C panic=abort -C opt-level=s \
             -C relocation-model=static -C code-model=kernel \
             -C target-feature=-red-zone,-mmx,-sse -C debuginfo=0 \
             --target x86_64-unknown-none

USRFLAGS  := -ffreestanding -fno-stack-protector -fno-pic -fno-pie -O2 \
             -nostdlib -nostartfiles -static -no-pie -mcmodel=large ${WARN} -mno-mmx -mno-sse

USER_LINK_BASE := 0x7f8000400000

KSRC_C  := kernel/main.c kernel/print.c kernel/isr.c kernel/syscall.c \
           kernel/task.c kernel/sched.c kernel/elf.c \
           kernel/mm/pmm.c kernel/mm/vmm.c kernel/mm/kheap.c \
           kernel/fs/vfs.c kernel/fs/ramfs.c kernel/fs/fat32.c \
           kernel/drivers/pci.c kernel/drivers/ahci.c \
           arch/cpu.c lib/string.c
KSRC_CXX:= kernel/mm/SlabAllocator.cpp kernel/drivers/Driver.cpp
KASM    := arch/entry64.S

UIMG    := init sh hello

KOBJ    := $(patsubst %.c,$(BUILD)/%.o,$(filter %.c,$(KSRC_C)))
KOBJ    += $(patsubst %.cpp,$(BUILD)/%.opp,$(filter %.cpp,$(KSRC_CXX)))
KOBJ    += $(BUILD)/arch/entry64.o
KOBJ    += $(BUILD)/initramfs.o

.PHONY: all clean run run-q35 test initramfs rust efi
all: $(BUILD)/bzImage

$(BUILD):
	mkdir -p $@

FORCE:
.PHONY: FORCE

# ---- Rust static library -------------------------------------------------
$(BUILD)/liblnxrm.a: kernel/rust/lib.rs kernel/rust/uart.rs kernel/rust/sync.rs kernel/rust/kalloc.rs kernel/rust/sysapi.rs | $(BUILD)
	@mkdir -p $(BUILD)/rustobj
	$(RUSTC) $(RUSTFLAGS) --crate-name lnxrmrust -o $@ kernel/rust/lib.rs

rust: $(BUILD)/liblnxrm.a

# ---- initramfs ------------------------------------------------------------
$(BUILD)/root.cpio: $(addprefix $(BUILD)/usr/,$(UIMG)) usr/motd.txt usr/console.txt
	$(MAKE) $(addprefix $(BUILD)/usr/,$(UIMG))
	$(PYTHON3) scripts/mkcpio.py $@ \
	  file=etc/motd@usr/motd.txt \
	  dir=dev char=dev/console \
	  dir=bin file=bin/init@$(BUILD)/usr/init \
	  file=bin/sh@$(BUILD)/usr/sh file=bin/hello@$(BUILD)/usr/hello

initramfs: $(BUILD)/root.cpio

$(BUILD)/initramfs.o: $(BUILD)/root.cpio
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.initramfs $< $@ \
	  --redefine-sym _binary_build_root_cpio_start=__initramfs_start \
	  --redefine-sym _binary_build_root_cpio_end=__initramfs_end \
	  --redefine-sym _binary_build_root_cpio_size=__initramfs_size

# user console placeholder (created by init at runtime if missing)
usr/console.txt:
	printf "lnxrm console\n" > $@
usr/motd.txt:
	printf "\n  Welcome to lnxrm!\n  A tiny unix-like kernel in asm+C+C+++Rust.\n\n" > $@

# ---- kernel objects --------------------------------------------------------
$(BUILD)/%.o: %.S | $(BUILD)
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 -F dwarf -g -o $@ $<

$(BUILD)/%.o: %.c FORCE | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(KERNFLAGS) -c -o $@ $<

$(BUILD)/%.opp: %.cpp FORCE | $(BUILD)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

# ---- final link ------------------------------------------------------------
$(BUILD)/vmlinux.elf: $(KOBJ) $(ARCHDIR)/kernel.ld $(BUILD)/liblnxrm.a
	$(LD) -T $(ARCHDIR)/kernel.ld -o $@ $(KOBJ) $(BUILD)/liblnxrm.a \
	      -Map $(BUILD)/vmlinux.map --no-warn-rwx-segments

$(BUILD)/vmlinux.bin: $(BUILD)/vmlinux.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/setup.bin: $(ARCHDIR)/setup.asm | $(BUILD)
	$(NASM) -f bin -o $@ $<
	python3 -c "d=open('$@','rb').read();pad=(-len(d))%512;open('$@','wb').write(d+b'\x00'*pad)"

$(BUILD)/bzImage: $(BUILD)/setup.bin $(BUILD)/vmlinux.bin
	cat $(BUILD)/setup.bin $(BUILD)/vmlinux.bin > $@
	$(PYTHON3) scripts/patch_bzimage.py $@ $$(( $$(stat -c%s $(BUILD)/setup.bin) ))
	ls -la $@

# ---- userspace -------------------------------------------------------------
$(BUILD)/usr/crt0.o: usr/crt0.S FORCE | $(BUILD)
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 -o $@ $<

$(BUILD)/usr/%.o: usr/%.c FORCE | $(BUILD)
	@mkdir -p $(dir $@)
	gcc $(USRFLAGS) -c -o $@ $<

$(BUILD)/usr/init: $(BUILD)/usr/init.o $(BUILD)/usr/ulib.o $(BUILD)/usr/crt0.o
	gcc $(USRFLAGS) -Wl,-Ttext=$(USER_LINK_BASE) -o $@ $^

$(BUILD)/usr/sh: $(BUILD)/usr/sh.o $(BUILD)/usr/ulib.o $(BUILD)/usr/crt0.o
	gcc $(USRFLAGS) -Wl,-Ttext=$(USER_LINK_BASE) -o $@ $^

$(BUILD)/usr/hello: $(BUILD)/usr/hello.o $(BUILD)/usr/ulib.o $(BUILD)/usr/crt0.o
	gcc $(USRFLAGS) -Wl,-Ttext=$(USER_LINK_BASE) -o $@ $^

# ---- convenience -----------------------------------------------------------
run: all
	qemu-system-x86_64 -m 256 -kernel $(BUILD)/bzImage -serial stdio -display none -no-reboot

run-vga: all
	qemu-system-x86_64 -m 256 -kernel $(BUILD)/bzImage -no-reboot

run-q35: all
	qemu-system-x86_64 -M q35 -m 256 -kernel $(BUILD)/bzImage -serial stdio \
	  -display none -no-reboot

test: all
	@printf 'help\nls /\nls /mnt\nps\ncat /etc/motd\n' | timeout 25 \
	  qemu-system-x86_64 -m 256 -kernel $(BUILD)/bzImage \
	  -serial stdio -display none -no-reboot 2>&1 | tee $(BUILD)/last.log || true

clean:
	rm -rf $(BUILD) usr/console.txt usr/motd.txt
