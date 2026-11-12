VERSION_MAJOR := 1
VERSION_MINOR := 2
VERSION_REVISION := 0
VERSIONS := VERSION_MAJOR=$(VERSION_MAJOR) VERSION_MINOR=$(VERSION_MINOR) VERSION_REVISION=$(VERSION_REVISION)

all:
	mkdir -p plugin/build app/data
	sed -e 's/VERSION_MAJOR/$(VERSION_MAJOR)/' -e 's/VERSION_MINOR/$(VERSION_MINOR)/' -e 's/VERSION_REVISION/$(VERSION_REVISION)/' plugin/relay.plgInfo > plugin/build/relay.plgInfo
	$(MAKE) -C plugin $(VERSIONS)
	cp plugin/relay.3gx app/data/plugin.bin
	$(MAKE) -C app cia $(VERSIONS)

clean:
	$(MAKE) -C plugin clean
	$(MAKE) -C app clean
	rm -rf app/data

re: clean all
