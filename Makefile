# zapret-gtk convenience targets. zapret itself is not in this repo: the
# installers and packages download the official release pinned in
# packaging/zapret-release.sh.

.PHONY: help run check install uninstall purge clean windows linux dist

help:
	@echo "targets:"
	@echo "  run        run the tray app from source (no install)"
	@echo "  check      byte-compile the Python sources"
	@echo "  install    run install.sh (deps, /opt/zapret, service, polkit, app)"
	@echo "  uninstall  run uninstall.sh"
	@echo "  purge      uninstall and remove /opt/zapret too"
	@echo "  windows    build the Windows installer (dist/Zapret-<version>-x64.msi)"
	@echo "  linux      build .deb and .rpm packages (amd64 + arm64) into dist/"
	@echo "  dist       both of the above (what the release workflow runs)"
	@echo "  clean      remove build output (keeps the downloaded zapret in build/cache)"

run:
	PYTHONPATH=src python3 -m zapret_tray

check:
	python3 -m py_compile src/zapret_tray/*.py

install:
	bash install.sh

uninstall:
	bash uninstall.sh

purge:
	bash uninstall.sh --purge

windows:
	bash windows/build.sh

linux:
	bash packaging/build-linux.sh

dist: windows linux

clean:
	rm -rf src/zapret_tray/__pycache__ build/windows build/linux
