# OpenMT SAMPLE-mode firmware for STM32F105 / F446 / F722
#
#   make                     F105 build with config.h as it is
#   make MCU=f446            STM32F446
#   make MCU=f722            STM32F722
#   make MCU=f446 FLASH_KB=256        (F446RC has 256K, F446RE 512K)
#   make APP_OFFSET=0x2000   if a bootloader lives at the start of flash
#   make LOADER=1            F105 behind the serial loader (see README): builds
#                            the loader too and a combined *_full.bin
#   make CFLAGS_EXTRA="-DHSE_HZ=12000000 -DEXT_CLK_HZ=12288000"
#
# Memory sizes:
#   F105:  R8 = 64K/20K,  RB = 128K/32K (default),  RC/VC = 256K/64K.
#          R8 build:  make FLASH_KB=64 RAM_KB=20 CFLAGS_EXTRA="-DTX_BUF_SAMPLES=4096"
#   F446:  RC = 256K/128K, RE = 512K/128K (default)
#   F722:  RC = 256K, RE = 512K (default) flash.  RAM is used from SRAM1
#          (SRAM1 + SRAM2, 192K at 0x20010000); the DTCM is not used because the DMA
#          cannot reach it.

MCU      ?= f105

PREFIX   ?= arm-none-eabi-
CC        = $(PREFIX)gcc
OBJCOPY   = $(PREFIX)objcopy
SIZE      = $(PREFIX)size

ifeq ($(MCU),f105)
  CPU        = -mcpu=cortex-m3 -mthumb
  MCU_DEFS   = -DMCU_F105 -DSTM32F10X_CL -Icmsis
  HW_SRC     = hw_f1.c
  FLASH_KB  ?= 128
  RAM_KB    ?= 32
  RAM_ORIGIN = 0x20000000
else ifeq ($(MCU),f446)
  CPU        = -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
  MCU_DEFS   = -DMCU_F446 -DSTM32F446xx -Icmsis/f4 -Icmsis/core
  HW_SRC     = hw_f4f7.c
  FLASH_KB  ?= 512
  RAM_KB    ?= 128
  RAM_ORIGIN = 0x20000000
else ifeq ($(MCU),f722)
  CPU        = -mcpu=cortex-m7 -mthumb -mfloat-abi=soft
  MCU_DEFS   = -DMCU_F722 -DSTM32F722xx -Icmsis/f7 -Icmsis/core
  HW_SRC     = hw_f4f7.c
  FLASH_KB  ?= 512
  RAM_KB    ?= 192
  RAM_ORIGIN = 0x20010000
else
  $(error MCU must be f105, f446 or f722)
endif

# LOADER=1: F105 behind the serial loader.  The loader takes the first 6 KB of
# flash (4 KB code + 2 KB marker page), the application starts at 0x1800.
LOADER   ?= 0
# status LED of the loader (optional), e.g. LOADER_LED_PORT=1 LOADER_LED_NUM=9 for PB9
LOADER_LED_PORT ?= -1
LOADER_LED_NUM  ?= 0
LOADER_DEBUG    ?= 0          # 1 = loader reports unexpected received bytes (see README)
ifeq ($(LOADER),1)
  ifneq ($(MCU),f105)
    $(error LOADER=1 is for the F105 only)
  endif
  APP_OFFSET ?= 0x1800
  ifneq ($(APP_OFFSET),0x1800)
    $(error LOADER=1 needs APP_OFFSET=0x1800)
  endif
  MCU_DEFS += -DUSE_LOADER=1
endif

APP_OFFSET ?= 0x0000

TARGET  = openmt_sample_$(MCU)
BUILD   = build/$(MCU)

SRCS    = startup.c main.c $(HW_SRC) engine.c clockcalc.c
OBJS    = $(SRCS:%.c=$(BUILD)/%.o)

CFLAGS  = $(CPU) -Os -g -std=gnu99 -Wall -Wextra -Wno-override-init \
          -ffunction-sections -fdata-sections -fno-common \
          $(MCU_DEFS) -I. $(CFLAGS_EXTRA)
LDFLAGS = $(CPU) -nostartfiles --specs=nano.specs --specs=nosys.specs \
          -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map \
          -T $(BUILD)/linker.ld

all: $(BUILD)/$(TARGET).bin $(BUILD)/$(TARGET).hex size

ifeq ($(LOADER),1)
# loader + application as one image for the first (SWD) flash at 0x08000000
all: $(BUILD)/$(TARGET)_full.bin

$(BUILD)/loader.bin: FORCE
	$(MAKE) -C loader FLASH_KB=$(FLASH_KB) RAM_KB=$(RAM_KB) \
	    LED_PORT=$(LOADER_LED_PORT) LED_NUM=$(LOADER_LED_NUM) LDR_DEBUG=$(LOADER_DEBUG)

$(BUILD)/$(TARGET)_full.bin: $(BUILD)/loader.bin $(BUILD)/$(TARGET).bin
	tr '\000' '\377' < /dev/zero | head -c $$(( $(APP_OFFSET) )) > $@
	dd if=$(BUILD)/loader.bin of=$@ conv=notrunc 2>/dev/null
	cat $(BUILD)/$(TARGET).bin >> $@
	$(OBJCOPY) -I binary -O ihex --change-addresses 0x08000000 $@ $(BUILD)/$(TARGET)_full.hex
endif

$(BUILD):
	mkdir -p $(BUILD)

# always regenerated, so APP_OFFSET / FLASH_KB / RAM_KB changes take effect
$(BUILD)/linker.ld: linker.ld.in FORCE | $(BUILD)
	$(CC) -E -P -x c \
	    -DFLASH_ORIGIN=$$((0x08000000 + $(APP_OFFSET))) \
	    -DFLASH_LEN_K=$$(( $(FLASH_KB) * 1024 - $(APP_OFFSET) )) \
	    -DRAM_ORIGIN=$(RAM_ORIGIN) \
	    -DRAM_LEN_K=$$(( $(RAM_KB) * 1024 )) $< > $@

$(BUILD)/%.o: %.c config.h hw.h engine.h clockcalc.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS) $(BUILD)/linker.ld
	$(CC) $(OBJS) $(LDFLAGS) -o $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

size: $(BUILD)/$(TARGET).elf
	$(SIZE) $<

clean:
	rm -rf build

.PHONY: all size clean FORCE
FORCE:
