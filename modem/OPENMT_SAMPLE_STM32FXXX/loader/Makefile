# Serial loader for the STM32F105 (see ../README.md, "Serial loader")
#
#   make -C loader                      128 KB flash / 32 KB RAM (F105RB)
#   make -C loader FLASH_KB=256 RAM_KB=64
#
# The loader must fit in 4 KB; the linker stops with an error if it does not.

PREFIX   ?= arm-none-eabi-
CC        = $(PREFIX)gcc
OBJCOPY   = $(PREFIX)objcopy
SIZE      = $(PREFIX)size

FLASH_KB ?= 128
RAM_KB   ?= 32

# optional status LED (on while waiting for the host, off once it is heard):
#   make LED_PORT=1 LED_NUM=9      for PB9 (port A=0, B=1, C=2 ...)
LED_PORT ?= -1
LED_NUM  ?= 0
LDR_DEBUG ?= 0

BUILD    = ../build/f105
CPU      = -mcpu=cortex-m3 -mthumb

CFLAGS   = $(CPU) -Os -g -std=gnu99 -Wall -Wextra -ffreestanding \
           -fno-tree-loop-distribute-patterns \
           -ffunction-sections -fdata-sections -fno-common \
           -DSTM32F10X_CL -I../cmsis -I. \
           -DLD_FLASH_KB=$(FLASH_KB) -DLD_RAM_KB=$(RAM_KB) \
           -DLD_LED_PORT=$(LED_PORT) -DLD_LED_NUM=$(LED_NUM) \
           -DLDR_DEBUG=$(LDR_DEBUG)
LDFLAGS  = $(CPU) -nostartfiles --specs=nano.specs --specs=nosys.specs \
           -Wl,--gc-sections -Wl,-Map=$(BUILD)/loader.map -T $(BUILD)/loader.ld

all: $(BUILD)/loader.bin size

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/loader.ld: loader.ld.in FORCE | $(BUILD)
	$(CC) -E -P -x c -DRAM_LEN_K=$$(( $(RAM_KB) * 1024 )) $< > $@

$(BUILD)/%.o: %.c loader.h FORCE | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/loader.elf: $(BUILD)/loader_core.o $(BUILD)/loader_hw.o $(BUILD)/loader.ld
	$(CC) $(BUILD)/loader_core.o $(BUILD)/loader_hw.o $(LDFLAGS) -o $@

$(BUILD)/loader.bin: $(BUILD)/loader.elf
	$(OBJCOPY) -O binary $< $@

size: $(BUILD)/loader.elf
	$(SIZE) $<

clean:
	rm -f $(BUILD)/loader.*  $(BUILD)/loader_*.o

.PHONY: all size clean FORCE
FORCE:
