# zapret-gtk

A tiny GTK app for GNOME whose **only job is one toggle**: turn
[zapret](https://github.com/bol-van/zapret) — a Deep Packet Inspection (DPI)
circumvention tool — **on or off**. It lives in the system tray and as a small
window with a switch.

```
   window                          tray menu (click the shield)
 ┌─ Zapret ───────────┐           🛡▾   filled = on · outline = off
 │       Zapret       │            ├ zapret: on
 │     ( ●——)  on     │            ├ ☑ Enable zapret
 │       Active       │            ├ ☑ Start at login
 │  ☑ Start at login  │            ├ Open Zapret…
 └────────────────────┘            └ Quit
```

zapret needs root (it runs `nfqws` and installs NFQUEUE firewall rules), so the
app never runs it directly. It drives a **`zapret.service`** systemd unit over the
system D-Bus, and a **polkit rule** scoped to that single unit lets your user
start/stop it **without a password**. The switch and icon also track the service's
real state, so changes made from the command line show up too.

## Features

- **One switch** — in the panel menu, in the window, or middle-click / scroll the
  tray icon to flip it instantly. Passwordless.
- **Icon reflects state** — filled shield when on, outlined when off.
- **Close to tray** — closing the window keeps it running in the tray; Quit from
  the tray menu to exit. (zapret itself keeps running regardless — it's a service.)
- **Start at login** — a per-user autostart entry you control from the app.
- **Remembers state** — your last on/off choice is restored when the app starts
  at login.

## How it fits together

```
 [zapret-gtk — your user]
        │  D-Bus → org.freedesktop.systemd1.Manager
        │     StartUnit / StopUnit ("zapret.service")   ← polkit rule = silent
        ▼
 [systemd] → zapret.service → /opt/zapret → nfqws + nftables NFQUEUE rules
```

- **`src/zapret_tray/`** — the GTK3 + PyGObject app.
  `app.py` (window + tray + menu), `systemd.py` (D-Bus start/stop/state),
  `autostart.py` (start-at-login), `state.py` (remembered on/off).
- **`zapret/`** — a vendored copy of bol-van/zapret (source, version-locked; built
  at install).
- **`data/`** — icons, the desktop/autostart entry, the polkit rule template, and
  the config overrides appended to zapret's config.
- **`install.sh` / `uninstall.sh` / `Makefile`** — setup.

## Requirements

- **Fedora** (the installer uses `dnf`), **GNOME Shell 45+**, x86_64. Works on
  Wayland or X11.
- On GNOME, the tray icon needs the **AppIndicator** extension
  (`gnome-shell-extension-appindicator`, pulled in by the installer). Without it
  the app still works as a window — GNOME just won't show the tray icon.

> Other desktops: the app speaks the cross-desktop StatusNotifierItem protocol, so
> the tray works **natively on KDE Plasma, XFCE, Cinnamon, etc.** (no extension).
> The app code is portable; only `install.sh` is Fedora/GNOME-specific.

The installer pulls build deps (`gcc make zlib-devel libcap-devel
libnetfilter_queue-devel libmnl-devel`) and runtime deps (`python3-gobject gtk3
libayatana-appindicator-gtk3 nftables curl`).

## Install

### Packages (easiest)

Download from [Releases](https://github.com/phuong74200/zapret-gtk/releases):

| System | Package |
|---|---|
| Windows 10/11 x64 | `Zapret-<version>-x64.msi` (see [Windows](#windows)) |
| Debian 12+, Ubuntu 24.04+ | `zapret-gtk_<version>-1_amd64.deb` / `_arm64.deb` |
| Fedora | `zapret-gtk-<version>-1.x86_64.rpm` / `.aarch64.rpm` |

```bash
sudo apt install ./zapret-gtk_*_amd64.deb      # Debian / Ubuntu
sudo dnf install ./zapret-gtk-*.x86_64.rpm     # Fedora
```

Then open **Zapret** and flip the switch; there's nothing else to set up. The
packages install the same pieces as `install.sh` (under `/usr` instead of
`/usr/local`), with zapret's prebuilt static binaries in `/opt/zapret` and the
bundled default strategy. Differences from `install.sh`:

- no interactive blockcheck step (run `sudo /opt/zapret/blockcheck.sh` later
  to tune, see below);
- the polkit rule lets **any user in an active local session** start/stop/restart
  `zapret.service` (a package can't know which user installed it).

If you used `install.sh` before, run `bash uninstall.sh` first.

### From source (Fedora)

```bash
git clone <this-repo> zapret-gtk
cd zapret-gtk
bash install.sh            # --yes to auto-confirm, --no-deps to skip dnf
```

zapret's source is **vendored in-tree** (version-locked with this app), so a plain
clone has everything — no submodule step.

During install you're offered **blockcheck**, zapret's tool that probes your
network and recommends a working DPI-bypass strategy. Run it and paste the
recommended `NFQWS_OPT` when prompted, or skip it — a solid default is bundled.

### First run on Wayland

GNOME on Wayland can't hot-reload a newly added shell extension, so the **first
time** you install the AppIndicator extension, enable it and **log out / back in**
for the tray icon to appear:

```bash
gnome-extensions enable appindicatorsupport@rgcjonas.gmail.com
# then log out / log back in
```

The window works immediately, with or without the tray.

## Use

Open **Zapret** (or it autostarts at login). Flip the switch → zapret starts with
**no password prompt**; the shield fills in. Flip off → it stops. You can also
middle-click or scroll the tray icon to toggle.

Verify:

```bash
systemctl is-active zapret     # "active" when on
journalctl -u zapret -n 20     # nfqws startup + nftables rules
```

## Tuning the DPI strategy

There is **no universal zapret config** — what defeats one ISP's DPI may not
defeat another's. Re-run blockcheck any time and edit the result into the config:

```bash
sudo /opt/zapret/blockcheck.sh
sudoedit /opt/zapret/config      # set NFQWS_OPT="..." near the bottom
```

`/opt/zapret/config` is seeded from zapret's `config.default`; this project
appends `NFQWS_ENABLE=1`, `FWTYPE=nftables`, and `MODE_FILTER=none` at the end.

## Run from source (no install)

```bash
make build                     # compile the vendored zapret
PYTHONPATH=src python3 -m zapret_tray
```

(Start/stop still needs `zapret.service` + the polkit rule installed.)

## Windows

A native Windows port lives in `windows/`: the same one-toggle tray app
(`zapret-tray.exe`, plain Win32/C) packaged as an **MSI** together with
zapret's Windows engine (`winws` + the WinDivert driver from the official
release). Install it by double-clicking the MSI; there is nothing else to set up.

| Linux                               | Windows                                         |
|-------------------------------------|-------------------------------------------------|
| `zapret.service` (systemd)          | `zapret` Windows service running `winws.exe`   |
| polkit rule for your user           | service permissions granted to the installing user |
| `/opt/zapret/config` (`NFQWS_OPT`)  | `C:\Program Files\Zapret\winws.conf`           |
| `~/.config/autostart` entry         | `HKCU\…\CurrentVersion\Run` value               |

Tray icon: left-click opens the window, right-click the menu, middle-click flips
zapret on/off. Windows 10/11 x64. See `windows/README.txt` (installed alongside
the app) for tuning and troubleshooting.

Build the installer on Linux (cross-compiled; no Windows machine needed):

```bash
sudo dnf install mingw64-gcc msitools        # Fedora
# sudo apt install gcc-mingw-w64-x86-64 wixl  # Debian/Ubuntu
make windows                                  # -> dist/Zapret-<version>-x64.msi
```

The build downloads the zapret release matching the vendored `zapret/` and
checks its SHA-256 (pinned in `packaging/zapret-release.sh`) before bundling it.

## Releases

`.github/workflows/release.yml` builds the MSI and the `.deb`/`.rpm` packages
(amd64 + arm64) and publishes them as a GitHub Release when a version tag is
pushed:

```bash
# 1. set the new version in VERSION and src/zapret_tray/__init__.py, then commit
git tag v0.1.2
git push origin main v0.1.2
```

The workflow refuses a tag that doesn't match `VERSION`. Running it by hand
(Actions → Release → Run workflow) builds everything without publishing; the
files are attached to the run as an artifact. Locally, `make dist` does the same
build (needs mingw-w64, wixl and [nfpm](https://nfpm.goreleaser.com)).

## Uninstall

```bash
bash uninstall.sh              # remove app, service, polkit rule, autostart
bash uninstall.sh --purge      # also delete /opt/zapret (config, host lists)
```

Packages: `sudo apt remove zapret-gtk` (`apt purge` also deletes `/opt/zapret`)
or `sudo dnf remove zapret-gtk`. Windows: Settings → Apps → Zapret.

## Troubleshooting

- **Tray icon missing (GNOME):** enable the AppIndicator extension and log out/in
  (see *First run on Wayland*). The window still works without it.
- **Toggle does nothing on left-click (GNOME):** that's expected — on GNOME a tray
  left-click opens the **menu**; use the menu's switch, or middle-click/scroll.
- **`zapret.service` fails with `203/EXEC`:** an SELinux labeling issue from
  copying into `/opt`. `install.sh` fixes it with `restorecon -RF /opt/zapret`; run
  that if you installed manually.

## Security notes

- The polkit rule (`/etc/polkit-1/rules.d/50-zapret.rules`) grants **only**
  `manage-units` for **`zapret.service`**, **only** to the installing user, and
  **only** from an active local session — no broader systemd access.
- D-Bus calls allow interactive authorization as a fallback: if the rule is ever
  missing, you get a normal polkit password prompt instead of a silent failure.

## License

This project: **MIT** (see `LICENSE`). Bundled zapret (`./zapret`): MIT, by
bol-van — see `zapret/docs/LICENSE.txt`.
