# Tailscale node: microlink + wireguard-lwip + lwIP (tunnel side) + port shims.
# `make host` builds build-host/libts.a and build-host/tsnode for the Mac.
# `make ctr` builds build-3ds/libts.a for the console; the Relay app links it, API port/ts_node.h.
# Sources come from the clones in ../../upstream (UP), pinned in private notes.

UP      ?= ../../upstream
ML      := $(UP)/microlink/components/microlink
WG      := $(ML)/components/wireguard_lwip/src
LWIP    := $(UP)/lwip/src
CJSON   := $(UP)/cjson
MBED    ?= $(UP)/mbedtls

ML_SRC := microlink.c ml_coord.c ml_coord_tls.c ml_derp.c ml_filter.c ml_h2.c ml_net_io.c \
          ml_noise.c ml_peer_nvs.c ml_routes.c ml_stun.c ml_tcp.c ml_udp.c ml_wg_mgr.c \
          ml_zerocopy.c nacl_box.c ml_at_validate.c
WG_SRC := wireguardif.c wireguard.c wireguard-filter.c crypto.c wireguard-platform-esp32.c \
          crypto/refc/x25519.c crypto/refc/chacha20.c crypto/refc/chacha20poly1305.c \
          crypto/refc/poly1305-donna.c crypto/refc/blake2s.c
LWIP_SRC := $(wildcard $(LWIP)/core/*.c $(LWIP)/core/ipv4/*.c $(LWIP)/core/ipv6/*.c) \
            $(LWIP)/api/tcpip.c $(LWIP)/api/err.c
PORT_SRC := port/rtos.c port/esp.c port/nvs.c port/sys_arch.c port/tls.c

SRC := $(addprefix $(ML)/src/,$(ML_SRC)) $(addprefix $(WG)/,$(WG_SRC)) $(LWIP_SRC) \
       $(CJSON)/cJSON.c $(PORT_SRC)

INC := -Iport/include -Iport -I$(ML)/include -I$(ML)/src -I$(WG) -I$(WG)/crypto/refc -I$(LWIP)/include \
       -I$(CJSON) -include port/include/ts_compat.h

# ---- host (Mac) ----
HOST_DIR := build-host
HOST_CC  ?= cc
HOST_CFLAGS := -DTS_PORT -std=gnu11 -g -O1 -Wall -Wno-unused-parameter -Wno-unused-function \
               -fsanitize=address,undefined -fno-omit-frame-pointer \
               $(INC) -I$(MBED)/include
HOST_OBJ := $(patsubst %.c,$(HOST_DIR)/%.o,$(subst $(UP)/,up/,$(SRC) port/os_posix.c))

host: $(HOST_DIR)/tsnode

$(HOST_DIR)/tsnode: host/tsnode.c $(HOST_DIR)/libts.a
	$(HOST_CC) $(HOST_CFLAGS) $< $(HOST_DIR)/libts.a -L$(MBED)/build/library \
		-lmbedtls -lmbedx509 -lmbedcrypto -fsanitize=address,undefined -o $@

$(HOST_DIR)/libts.a: $(HOST_OBJ)
	rm -f $@ && ar rcs $@ $^

$(HOST_DIR)/up/%.o: $(UP)/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

$(HOST_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

# ---- console (3DS, devkitARM + libctru + portlib mbedTLS 2.28.8) ----
CTR_DIR := build-3ds
DKP     ?= $(DEVKITPRO)
CTR_BIN := $(DKP)/devkitARM/bin/arm-none-eabi-
CTR_ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CTR_CFLAGS := -DTS_PORT -D__3DS__ -std=gnu11 -g -O2 -mword-relocations -ffunction-sections \
              -Wall -Wno-unused-parameter -Wno-unused-function $(CTR_ARCH) \
              $(INC) -I$(DKP)/portlibs/3ds/include -I$(DKP)/libctru/include
CTR_OBJ := $(patsubst %.c,$(CTR_DIR)/%.o,$(subst $(UP)/,up/,$(SRC) port/os_ctru.c port/node.c port/serve.c))

ctr: $(CTR_DIR)/libts.a

$(CTR_DIR)/libts.a: $(CTR_OBJ)
	rm -f $@ && $(CTR_BIN)ar rcs $@ $^

$(CTR_DIR)/up/%.o: $(UP)/%.c
	@mkdir -p $(dir $@)
	$(CTR_BIN)gcc $(CTR_CFLAGS) -c $< -o $@

$(CTR_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CTR_BIN)gcc $(CTR_CFLAGS) -c $< -o $@

clean:
	rm -rf $(HOST_DIR) $(CTR_DIR)

.PHONY: host ctr clean
