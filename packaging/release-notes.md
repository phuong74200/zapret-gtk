One switch to turn [zapret](https://github.com/bol-van/zapret) (DPI circumvention) on or off, from the tray or a small window. zapret is bundled, so you don't need to set it up yourself.

## Download

| System | File |
|---|---|
| Windows 10 / 11 (64-bit) | `Zapret-{{VERSION}}-x64.msi` |
| Debian 12+, Ubuntu 24.04+ (x86_64) | `zapret-gtk_{{VERSION}}-1_amd64.deb` |
| Debian 12+, Ubuntu 24.04+ (ARM64) | `zapret-gtk_{{VERSION}}-1_arm64.deb` |
| Fedora (x86_64) | `zapret-gtk-{{VERSION}}-1.x86_64.rpm` |
| Fedora (ARM64) | `zapret-gtk-{{VERSION}}-1.aarch64.rpm` |

## Install

**Windows:** run the `.msi`. If SmartScreen warns about an unknown publisher (the installer isn't code-signed), choose *More info → Run anyway*. Running a newer `.msi` upgrades the installed version.

Microsoft Defender may flag the app with a machine-learning detection (names ending in `!ml`). That's a false positive common for new, unsigned programs, and zapret tools in particular, since they intercept network traffic. If it happens, open Windows Security → Protection history, pick the item, and choose *Allow on device*, then run the installer again. The source for everything in the installer is in this repository.

**Debian / Ubuntu:**

```bash
sudo apt install ./zapret-gtk_{{VERSION}}-1_amd64.deb
```

**Fedora:**

```bash
sudo dnf install ./zapret-gtk-{{VERSION}}-1.x86_64.rpm
```

Then open **Zapret** from your apps and flip the switch. On GNOME the tray icon needs the AppIndicator extension (installed as a recommended package); log out and back in once after installing it.

If you used `install.sh` before, run `bash uninstall.sh` first.

If the default strategy doesn't work with your ISP, tune it: on Linux edit `NFQWS_OPT` in `/opt/zapret/config` (run `sudo /opt/zapret/blockcheck.sh` to find one); on Windows edit `C:\Program Files\Zapret\winws.conf`.

`SHA256SUMS.txt` lists the checksums of all files.
