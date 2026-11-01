# Open Doctrines on FreeRTOS.
#
#   make BOARD=mps2-an385 FREERTOS_KERNEL=/path/to/FreeRTOS-Kernel
#   make BOARD=stm32f405  ...
#   make BOARD=riscv32-virt ...
#   make BOARD=posix ...
#   make host                    the core alone, no kernel (tests use it)
#   make run BOARD=...           and start it in QEMU (or on the host)
#
# FREERTOS_KERNEL is a checkout of github.com/FreeRTOS/FreeRTOS-Kernel at the
# tag in KERNEL_TAG; tools/freertos_kernel.sh fetches exactly that.
BOARD ?= mps2-an385
KERNEL_TAG := V11.1.0
FREERTOS_KERNEL ?= ../build/FreeRTOS-Kernel
ROOT := ..
OUT := $(ROOT)/build/freertos/$(BOARD)
PY ?= python3
HOSTCC ?= cc

CORE := core/od_world.c core/od_rules.c core/od_shell.c
WORLD := $(OUT)/od_world_data.c
K := $(abspath $(FREERTOS_KERNEL))
KSRC := $(K)/tasks.c $(K)/list.c $(K)/queue.c $(K)/portable/MemMang/heap_4.c
CFLAGS_COMMON := -std=gnu99 -Os -ffunction-sections -fdata-sections \
    -I$(CURDIR)/core -I$(CURDIR)/ports/$(BOARD) -I$(abspath $(ROOT)/templeos) -I$(K)/include
# The game is held to -Werror; the kernel is somebody else's code and is not.
WARN := -Wall -Wextra -Werror

ifeq ($(BOARD),mps2-an385)
  CC := arm-none-eabi-gcc
  ARCH := -mcpu=cortex-m3 -mthumb
  PORT := $(K)/portable/GCC/ARM_CM3
  BSRC := ports/cortex-m/startup.c ports/cortex-m/semihost.c ports/cortex-m/syscalls.c
  LDFLAGS := -Tports/$(BOARD)/link.ld -Lports/cortex-m --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections
  QEMU := qemu-system-arm -M mps2-an385 -semihosting-config enable=on,target=native
else ifeq ($(BOARD),stm32f405)
  CC := arm-none-eabi-gcc
  ARCH := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
  PORT := $(K)/portable/GCC/ARM_CM4F
  BSRC := ports/cortex-m/startup.c ports/cortex-m/semihost.c ports/cortex-m/syscalls.c
  LDFLAGS := -Tports/$(BOARD)/link.ld -Lports/cortex-m --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections
  QEMU := qemu-system-arm -M netduinoplus2 -semihosting-config enable=on,target=native
else ifeq ($(BOARD),riscv32-virt)
  CC := riscv64-unknown-elf-gcc
  ARCH := -march=rv32imac_zicsr_zifencei -mabi=ilp32 -mcmodel=medany --specs=picolibc.specs
  PORT := $(K)/portable/GCC/RISC-V
  BSRC := ports/$(BOARD)/start.S $(PORT)/portASM.S
  CFLAGS_COMMON += -I$(PORT)/chip_specific_extensions/RISCV_MTIME_CLINT_no_extensions
  LDFLAGS := -Tports/$(BOARD)/link.ld -nostartfiles -Wl,--gc-sections,--no-warn-rwx-segments
  QEMU := qemu-system-riscv32 -M virt -bios none
else ifeq ($(BOARD),posix)
  CC ?= cc
  ARCH :=
  PORT := $(K)/portable/ThirdParty/GCC/Posix
  BSRC := $(PORT)/utils/wait_for_event.c
  CFLAGS_COMMON += -I$(PORT)/utils
  LDFLAGS := -pthread
else
  $(error BOARD is mps2-an385, stm32f405, riscv32-virt or posix)
endif

ELF := $(OUT)/opendoctrines.elf

all: $(ELF)

$(WORLD): $(ROOT)/templeos/world.odw $(ROOT)/tools/freertos_world.py
	@mkdir -p $(OUT)
	$(PY) $(ROOT)/tools/freertos_world.py $< $@

$(ELF): $(CORE) core/od_main.c ports/$(BOARD)/board.c $(BSRC) $(WORLD) $(wildcard core/*.h ports/$(BOARD)/*.h)
	@test -f $(K)/tasks.c || { echo "no FreeRTOS kernel at $(K): tools/freertos_kernel.sh fetches $(KERNEL_TAG)"; exit 1; }
	@mkdir -p $(OUT)/kernel
	cd $(OUT)/kernel && $(CC) $(ARCH) $(CFLAGS_COMMON) -I$(PORT) -c \
	    $(KSRC) $(PORT)/port.c $(abspath $(filter %.S $(K)/%,$(BSRC)))
	$(CC) $(ARCH) $(CFLAGS_COMMON) $(WARN) -I$(PORT) $(CORE) core/od_main.c ports/$(BOARD)/board.c \
	    $(filter-out %.S $(K)/%,$(BSRC)) $(WORLD) $(OUT)/kernel/*.o $(LDFLAGS) -o $@
	@if [ "$(BOARD)" != posix ]; then $(CC:gcc=size) $@; fi
	@if [ "$(BOARD)" = stm32f405 ]; then $(CC:gcc=objcopy) -O binary $@ $(OUT)/opendoctrines.bin; fi

HOST := $(ROOT)/build/freertos/host/od_host
host: $(HOST)
$(HOST): $(CORE) host/od_host.c $(wildcard core/*.h)
	@mkdir -p $(dir $@)
	$(PY) $(ROOT)/tools/freertos_world.py $(ROOT)/templeos/world.odw $(dir $@)od_world_data.c
	$(HOSTCC) -std=c99 -O2 -Wall -Wextra -Werror -Icore -I$(ROOT)/templeos $(CORE) host/od_host.c \
	    $(dir $@)od_world_data.c -o $@

run: $(ELF)
ifeq ($(BOARD),posix)
	$(ELF)
else
	$(QEMU) -display none -monitor none -serial stdio -kernel $(ELF)
endif

.PHONY: all host run
