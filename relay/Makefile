VERSION_MAJOR := 1
VERSION_MINOR := 3
VERSION_REVISION := 3
VERSIONS := VERSION_MAJOR=$(VERSION_MAJOR) VERSION_MINOR=$(VERSION_MINOR) VERSION_REVISION=$(VERSION_REVISION)

# common/build.h: "<commit>[+]@<yymmddhhmm>", "+" for a dirty tree; the app's updater orders builds by
# the stamp after '@'. Rewritten only on change so make rebuilds what includes it.
BUILD := $(shell git rev-parse --short HEAD)$(shell git diff --quiet HEAD || echo +)@$(shell date +%y%m%d%H%M)

# CA roots for the app's HTTPS (GitHub), embedded at build time; Linux: CA=/etc/ssl/certs/ca-certificates.crt
CA ?= /etc/ssl/cert.pem

all:
	mkdir -p plugin/build app/data app/certs
	cmp -s $(CA) app/certs/cacert.pem || cp $(CA) app/certs/cacert.pem
	echo '#define RELAY_BUILD "$(BUILD)"' > common/build.h.new
	cmp -s common/build.h.new common/build.h || mv common/build.h.new common/build.h; rm -f common/build.h.new
	sed -e 's/VERSION_MAJOR/$(VERSION_MAJOR)/' -e 's/VERSION_MINOR/$(VERSION_MINOR)/' -e 's/VERSION_REVISION/$(VERSION_REVISION)/' plugin/relay.plgInfo > plugin/build/relay.plgInfo
	$(MAKE) -C plugin $(VERSIONS)
	cp plugin/relay.3gx app/data/plugin.bin
	$(MAKE) -C app cia $(VERSIONS)

# Uploads this build as the latest GitHub release of SolRaze/trey, where the app's updater looks
# (update.hpp UPDATE_BASE). Tag relay-<yymmddhhmm>; the tree should be committed and pushed first.
release: all
	echo '$(BUILD)' > app/build
	gh release create relay-$(lastword $(subst @, ,$(BUILD))) app/relay.cia app/build --repo SolRaze/trey \
		--title 'relay $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_REVISION)' --notes '$(BUILD)' --latest
	rm -f app/build

clean:
	$(MAKE) -C plugin clean
	$(MAKE) -C app clean
	rm -rf app/data

re: clean all

.PHONY: all release clean re
