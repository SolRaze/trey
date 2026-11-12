VERSION_MAJOR := 1
VERSION_MINOR := 3
VERSION_REVISION := 0
VERSIONS := VERSION_MAJOR=$(VERSION_MAJOR) VERSION_MINOR=$(VERSION_MINOR) VERSION_REVISION=$(VERSION_REVISION)

# common/build.h names the commit, "+" when the tree is dirty; rewritten only on change so make rebuilds what includes it.
BUILD := $(shell git rev-parse --short HEAD)$(shell git diff --quiet HEAD || echo +)

all:
	mkdir -p plugin/build app/data
	echo '#define RELAY_BUILD "$(BUILD)"' > common/build.h.new
	cmp -s common/build.h.new common/build.h || mv common/build.h.new common/build.h; rm -f common/build.h.new
	sed -e 's/VERSION_MAJOR/$(VERSION_MAJOR)/' -e 's/VERSION_MINOR/$(VERSION_MINOR)/' -e 's/VERSION_REVISION/$(VERSION_REVISION)/' plugin/relay.plgInfo > plugin/build/relay.plgInfo
	$(MAKE) -C plugin $(VERSIONS)
	cp plugin/relay.3gx app/data/plugin.bin
	$(MAKE) -C app cia $(VERSIONS)

clean:
	$(MAKE) -C plugin clean
	$(MAKE) -C app clean
	rm -rf app/data

re: clean all
