# zapret-gtk convenience targets. The bundled zapret has its own Makefile under
# ./zapret; this one wraps the dev/install workflow.

.PHONY: help build run check install uninstall purge clean

help:
	@echo "targets:"
	@echo "  build      compile the vendored zapret (nfqws, tpws, ...) in ./zapret"
	@echo "  run        run the tray app from source (no install)"
	@echo "  check      byte-compile the Python sources"
	@echo "  install    run install.sh (deps, /opt/zapret, service, polkit, app)"
	@echo "  uninstall  run uninstall.sh"
	@echo "  purge      uninstall and remove /opt/zapret too"
	@echo "  clean      clean zapret build artifacts and __pycache__"

build:
	$(MAKE) -C zapret

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

clean:
	-$(MAKE) -C zapret clean
	rm -rf src/zapret_tray/__pycache__
