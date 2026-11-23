# core.cxi: the sysmodule Relay copies to /luma/sysmodules/0004013000F0E702.cxi and launches.
# Linked like a 3dsx (same 0x100000 layout), wrapped by makerom as an NCCH.
include $(DEVKITARM)/base_tools

CTRU   := $(DEVKITPRO)/libctru
ARCH   := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS := -g -Os -std=gnu11 -Wall -Wextra -mword-relocations -ffunction-sections -fdata-sections \
          $(ARCH) -D__3DS__ -I$(CTRU)/include -I../common
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -Wl,--gc-sections,-Map,core.map -L$(CTRU)/lib
SRC    := $(wildcard source/*.c)

# No -nocodepadding: Luma's loader maps ro and data at page offsets of the decompressed .code;
# packed segments shift them and every rodata string and data value reads wrong.
core.cxi: core.elf core.rsf
	makerom -f ncch -rsf core.rsf -o $@ -elf $<

core.elf: $(SRC) $(wildcard source/*.h) ../common/build.h
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRC) -lctru -lm -o $@

clean:
	rm -f core.cxi core.elf core.map

.PHONY: clean
