.RECIPEPREFIX := >

ifeq ($(strip $(DEVKITARM)),)
$(error DEVKITARM is not set)
endif
ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set)
endif

TARGET  := PSXctr
CORELIB := core/pcsx_rearmed_libretro_ctr.a
CTRULIB := $(DEVKITPRO)/libctru

CC      := $(DEVKITARM)/bin/arm-none-eabi-gcc
ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS  := -g -Wall -O2 -mword-relocations -ffunction-sections $(ARCH) \
           -D__3DS__ -I$(CTRULIB)/include -Icore/deps/libretro-common/include
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(TARGET).map
LIBS    := $(CORELIB) -L$(CTRULIB)/lib -lctru -lm

.PHONY: all clean

all: $(TARGET).3dsx

$(TARGET).elf: source/main.c $(CORELIB)
> $(CC) $(CFLAGS) $(LDFLAGS) source/main.c $(LIBS) -o $@

$(TARGET).3dsx: $(TARGET).elf
> $(DEVKITPRO)/tools/bin/3dsxtool $< $@

clean:
> rm -f $(TARGET).elf $(TARGET).3dsx $(TARGET).map
