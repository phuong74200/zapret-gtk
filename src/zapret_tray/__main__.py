"""Entry point: ``python -m zapret_tray`` (or the installed ``zapret-tray``)."""

import sys

from .app import ZapretApp


def main():
    return ZapretApp().run(sys.argv)


if __name__ == "__main__":
    raise SystemExit(main())
