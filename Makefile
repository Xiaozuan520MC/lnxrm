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

WARN    := -w
KERNFLAGS := -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
             -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel \
             -O2 -g $(WARN) -std=gnu11 -Iinclude -Ilib -I. -MMD -MP
CXXFLAGS  := $(KERNFLAGS) -std=c++20 -fno-exceptions -fno-rtti \
             -fno-threadsafe-statics -fno-use-cxa-atexit -nostdinc++

RUSTFLAGS := --crate-type staticlib --edition 2021 -C panic=abort -C opt-level=s \
             -C relocation-model=static -C code-model=kernel \
             -C target-feature=-red-zone,-mmx,-sse -C debuginfo=0 \
             --target x86_64-unknown-none \
             -A warnings

USRFLAGS  := -ffreestanding -fno-stack-protector -fno-pic -fno-pie -O2 \
             -nostdlib -nostartfiles -static -no-pie -mcmodel=large $(WARN) -mno-mmx -mno-sse -Iinit

USER_LINK_BASE := 0x7f8000400000

KSRC_C  := kernel/main.c kernel/print.c kernel/isr.c kernel/syscall.c \
           kernel/task.c kernel/sched.c kernel/elf.c \
           kernel/mm/pmm.c kernel/mm/vmm.c kernel/mm/kheap.c \
           kernel/fs/vfs.c kernel/fs/ramfs.c kernel/fs/fat32.c kernel/fs/blk_cache.c \
           kernel/fs/fat32_journal.c kernel/drivers/pci.c kernel/drivers/ahci.c \
           kernel/apic.c kernel/smp.c kernel/percpu.c kernel/signal.c kernel/spinlock.c \
           arch/cpu.c lib/string.c
KSRC_CXX:= kernel/mm/SlabAllocator.cpp kernel/drivers/Driver.cpp
KASM    := arch/entry64.S arch/trampoline.S arch/signal_trampoline.S

UIMG    := init sh hello

KOBJ_C  := $(patsubst %.c,%.o,$(filter %.c,$(KSRC_C)))
KOBJ_CXX:= $(patsubst %.cpp,%.opp,$(filter %.cpp,$(KSRC_CXX)))
KOBJ_ASM:= arch/entry64.o $(BUILD)/trampoline.o arch/signal_trampoline.o
KOBJ    := $(KOBJ_C) $(KOBJ_CXX) $(KOBJ_ASM) $(BUILD)/initramfs.o

.PHONY: all clean run run-only run-q35 test initramfs rust efi
all: $(BUILD)/bzImage

$(BUILD):
	mkdir -p $@

FORCE:
.PHONY: FORCE

$(BUILD)/liblnxrm.a: kernel/rust/lib.rs kernel/rust/uart.rs kernel/rust/sync.rs kernel/rust/kalloc.rs kernel/rust/sysapi.rs | $(BUILD)
	@mkdir -p $(BUILD)/rustobj
	$(RUSTC) $(RUSTFLAGS) --crate-name lnxrmrust -o $@ kernel/rust/lib.rs

rust: $(BUILD)/liblnxrm.a

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

usr/console.txt:
	printf "lnxrm console\n" > $@
usr/motd.txt:
	printf "\n  Welcome to lnxrm!\n  A tiny unix-like kernel in asm+C+C+++Rust.\n\n" > $@

arch/entry64.o: arch/entry64.S | $(BUILD)
	$(NASM) -f elf64 -F dwarf -g -o $@ $<

$(BUILD)/trampoline.bin: arch/trampoline.S | $(BUILD)
	$(NASM) -f bin -o $@ $<

$(BUILD)/trampoline.o: $(BUILD)/trampoline.bin | $(BUILD)
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.trampoline $< $@ \
	  --redefine-sym _binary_build_trampoline_bin_start=trampoline_start \
	  --redefine-sym _binary_build_trampoline_bin_end=trampoline_end \
	  --redefine-sym _binary_build_trampoline_bin_size=trampoline_size

arch/signal_trampoline.o: arch/signal_trampoline.S | $(BUILD)
	$(NASM) -f elf64 -F dwarf -g -o $@ $<

%.o: %.c FORCE | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(KERNFLAGS) -MMD -MP -c -o $@ $<

%.opp: %.cpp FORCE | $(BUILD)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

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

usr/crt0.o: init/crt0.S FORCE
	$(NASM) -f elf64 -o $@ $<

usr/ulib.o: init/ulib.c FORCE
	gcc $(USRFLAGS) -c -o $@ $<

usr/init.o: usr/init.c FORCE
	gcc $(USRFLAGS) -c -o $@ $<

usr/sh.o: usr/sh.c FORCE
	gcc $(USRFLAGS) -c -o $@ $<

usr/hello.o: usr/hello.c FORCE
	gcc $(USRFLAGS) -c -o $@ $<

$(BUILD)/usr/init: usr/init.o usr/ulib.o usr/crt0.o | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(USRFLAGS) -Wl,-Ttext=$(USER_LINK_BASE) -o $@ $^

$(BUILD)/usr/sh: usr/sh.o usr/ulib.o usr/crt0.o | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(USRFLAGS) -Wl,-Ttext=$(USER_LINK_BASE) -o $@ $^

$(BUILD)/usr/hello: usr/hello.o usr/ulib.o usr/crt0.o | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(USRFLAGS) -Wl,-Ttext=$(USER_LINK_BASE) -o $@ $^

run:
	qemu-system-x86_64 -m 256 -smp 2 -kernel $(BUILD)/bzImage -no-reboot -serial stdio -d int -D qemu.log

clean:
	rm -rf $(BUILD) usr/console.txt usr/motd.txt
	rm -f $(KOBJ_C) $(KOBJ_CXX) $(KOBJ_ASM)
	rm -f arch/signal_trampoline.o
	rm -f usr/crt0.o usr/init.o usr/sh.o usr/hello.o usr/ulib.o
	rm -f $(KSRC_C:.c=.d) $(KSRC_CXX:.cpp=.d)
	rm -f kernel/rust/*.o kernel/rust/*.rmeta
	rm -f qemu.log
