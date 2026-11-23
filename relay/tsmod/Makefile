# tsmod.cxi: the Tailscale sysmodule Relay copies to /luma/sysmodules/0004013000F0E802.cxi and launches.
# Linked like a 3dsx against ../ts/build-3ds/libts.a (make -C ../ts ctr first), wrapped by makerom as an NCCH.
include $(DEVKITARM)/base_tools

CTRU   := $(DEVKITPRO)/libctru
PORT   := $(DEVKITPRO)/portlibs/3ds
ARCH   := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS := -g -Os -std=gnu11 -Wall -Wextra -mword-relocations -ffunction-sections -fdata-sections \
          $(ARCH) -D__3DS__ -I$(CTRU)/include -I../common -I../ts/port
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -Wl,--gc-sections,-Map,tsmod.map -L$(CTRU)/lib -L$(PORT)/lib
SRC    := $(wildcard source/*.c)
LIBTS  := ../ts/build-3ds/libts.a

# No -nocodepadding: Luma's loader maps ro and data at page offsets of the decompressed .code (see core/).
tsmod.cxi: tsmod.elf tsmod.rsf
	makerom -f ncch -rsf tsmod.rsf -o $@ -elf $<

tsmod.elf: $(SRC) $(LIBTS) ../common/build.h ../common/core.h
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRC) $(LIBTS) -lmbedtls -lmbedx509 -lmbedcrypto -lctru -lm -o $@

clean:
	rm -f tsmod.cxi tsmod.elf tsmod.map

.PHONY: clean
