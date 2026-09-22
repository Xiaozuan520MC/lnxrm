CROSS   :=
CC      := $(CROSS)gcc
CXX     := $(CROSS)g++
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
RUSTC   := rustc
NASM    := nasm -w-implicit-abs-deprecated
PYTHON3 := python3

BUILD   := build
ARCHDIR := arch

WARN    := -w
MAKEFLAGS += --no-print-directory
KERNFLAGS := -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
             -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel \
             -O2 -g $(WARN) -std=gnu11 -Iinclude -Ilib -I. -MMD -MP
CXXFLAGS  := $(KERNFLAGS) -std=c++20 -fno-exceptions -fno-rtti \
             -fno-threadsafe-statics -fno-use-cxa-atexit -nostdinc++

RUSTFLAGS := --crate-type staticlib --edition 2021 -C panic=abort -C opt-level=s \
             -C relocation-model=static -C code-model=kernel \
             -C target-feature=-mmx,-sse -C debuginfo=0 \
             --target x86_64-unknown-none \
             -A warnings

USRFLAGS  := -ffreestanding -fno-stack-protector -fno-pic -fno-pie -O2 \
             -nostdlib -nostartfiles -static -no-pie -mcmodel=large $(WARN) -mno-mmx -mno-sse -Iinit

USER_LINK_BASE := 0x7f8000400000

KSRC_C   := $(shell find kernel arch lib -name '*.c' 2>/dev/null)
KSRC_CXX := $(shell find kernel arch lib -name '*.cpp' 2>/dev/null)
KASM_ALL := $(shell find arch -name '*.S' 2>/dev/null)
KASM     := $(filter-out arch/trampoline.S,$(KASM_ALL))
RUST_SRC := $(shell find kernel/rust -name '*.rs' 2>/dev/null)
USR_ALL_C := $(shell find usr -maxdepth 1 -name '*.c' 2>/dev/null)
USR_LIB_C := usr/ulib.c
UIMG      := $(patsubst usr/%.c,%,$(filter-out $(USR_LIB_C),$(USR_ALL_C)))

KOBJ_C   := $(patsubst %.c,$(BUILD)/%.o,$(KSRC_C))
KOBJ_CXX := $(patsubst %.cpp,$(BUILD)/%.opp,$(KSRC_CXX))
KOBJ_ASM := $(patsubst %.S,$(BUILD)/%.o,$(KASM)) $(BUILD)/trampoline.o
KOBJ     := $(KOBJ_C) $(KOBJ_CXX) $(KOBJ_ASM)

.PHONY: all clean run
all: $(BUILD)/bzImage

$(BUILD):
	@mkdir -p $@

FORCE:
.PHONY: FORCE

$(BUILD)/liblnxrm.a: $(RUST_SRC) | $(BUILD)
	@mkdir -p $(BUILD)/rustobj
	@printf "  RUST    $@\n"
	@$(RUSTC) $(RUSTFLAGS) --crate-name lnxrmrust -o $@ kernel/rust/lib.rs

rust: $(BUILD)/liblnxrm.a

$(BUILD)/trampoline.bin: arch/trampoline.S | $(BUILD)
	@printf "  NASM    $@\n"
	@$(NASM) -f bin -o $@ $<

$(BUILD)/trampoline.o: $(BUILD)/trampoline.bin | $(BUILD)
	@printf "  OBJCOPY $@\n"
	@$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.trampoline $< $@ \
	  --redefine-sym _binary_build_trampoline_bin_start=trampoline_start \
	  --redefine-sym _binary_build_trampoline_bin_end=trampoline_end \
	  --redefine-sym _binary_build_trampoline_bin_size=trampoline_size

$(BUILD)/arch/%.o: arch/%.S | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  NASM    $@\n"
	@$(NASM) -f elf64 -F dwarf -g -o $@ $<

$(BUILD)/%.o: %.c | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  CC      $@\n"
	@$(CC) $(KERNFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/%.opp: %.cpp | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  CXX     $@\n"
	@$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/vmlinux.elf: $(KOBJ) $(ARCHDIR)/kernel.ld $(BUILD)/liblnxrm.a
	@printf "  LD      $@\n"
	@$(LD) -T $(ARCHDIR)/kernel.ld -o $@ $(KOBJ) $(BUILD)/liblnxrm.a \
	      -Map $(BUILD)/vmlinux.map --no-warn-rwx-segments

$(BUILD)/vmlinux.bin: $(BUILD)/vmlinux.elf
	@printf "  OBJCOPY $@\n"
	@$(OBJCOPY) -O binary $< $@
	
$(BUILD)/setup.bin: $(ARCHDIR)/setup.asm $(BUILD)/vmlinux.elf | $(BUILD)
	@printf "  NASM    $@\n"
	@$(NASM) -f bin -o $@ $<
	@python3 -c "d=open('$@','rb').read();pad=(2048-len(d)%2048)%2048;open('$@','wb').write(d+b'\x00'*pad)"
	@$(PYTHON3) scripts/patch_setup_jmp.py $@ $(BUILD)/vmlinux.elf

$(BUILD)/bzImage: $(BUILD)/setup.bin $(BUILD)/vmlinux.bin
	@printf "  BZIMAGE $@\n"
	@cat $(BUILD)/setup.bin $(BUILD)/vmlinux.bin > $@
	@$(PYTHON3) scripts/patch_bzimage.py $@ $$(( $$(stat -c%s $(BUILD)/setup.bin) ))
	@printf "  SIZE    %s bytes\n" $$(stat -c%s $@)
	@printf "  Done!\n"

usr/crt0.o: init/crt0.S
	@printf "  NASM    $@\n"
	@$(NASM) -f elf64 -o $@ $<

usr/ulib.o: init/ulib.c
	@printf "  CC      $@\n"
	@gcc $(USRFLAGS) -c -o $@ $<

usr/%.o: usr/%.c
	@printf "  CC      $@\n"
	@gcc $(USRFLAGS) -c -o $@ $<

$(addprefix $(BUILD)/usr/,$(UIMG)): $(BUILD)/usr/%: usr/%.o usr/ulib.o usr/crt0.o | $(BUILD)
	@mkdir -p $(dir $@)
	@printf "  LD      $@\n"
	@$(CC) $(USRFLAGS) -Wl,-Ttext=$(USER_LINK_BASE) -o $@ $^

clean:
	@rm -rf $(BUILD)
	@echo Done!

run: $(BUILD)/disk.img
	@qemu-system-x86_64 -m 256 -smp 2 -kernel $(BUILD)/bzImage \
	  -drive file=$(BUILD)/disk.img,format=raw,if=ide,index=0,media=disk \
	  -serial stdio -no-reboot -d int -D build/qemu.log
	  
$(BUILD)/disk.img: $(BUILD)/README.md $(addprefix $(BUILD)/usr/,$(UIMG))
	@printf "  DISK    $@\n"
	@dd if=/dev/zero of=$@ bs=1M count=64 2>/dev/null
	@mkfs.vfat -F 32 $@ 2>/dev/null
	@mcopy -i $@ $(BUILD)/README.md ::README.md
	@mmd -i $@ ::bin
	@for f in $(addprefix $(BUILD)/usr/,$(UIMG)); do \
	  mcopy -i $@ $$f ::bin/$$(basename $$f); \
	done

$(BUILD)/README.md: | $(BUILD)
	@echo "A tiny unix-like kernel in ASM + C + C++ + Rust!" >> $@
