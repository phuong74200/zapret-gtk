Zapret for Windows
==================

A tray app with one job: turn zapret (DPI circumvention) on or off.

  Tray icon     green shield = on, grey outline = off
  Left-click    open the window with the on/off switch
  Right-click   menu: Enable zapret, Start at login, Open, Quit
  Middle-click  flip zapret on/off

Closing the window keeps the app in the tray. zapret itself runs as the
Windows service "zapret", so it keeps running even if you quit the tray app.
The app remembers your last on/off choice and restores it at login.

No administrator rights are needed to toggle: the installer gave your
account permission to start and stop the "zapret" service. Other accounts
on this PC can see the state but not change it (reinstall from that account
to give it access).


Tuning the strategy
-------------------

There is no universal zapret config: what defeats one ISP's DPI may not
defeat another's. The options winws runs with are in:

    C:\Program Files\Zapret\winws.conf

Edit it with an editor run as administrator, then switch zapret off and on
again to apply. Lines are joined with spaces and lines starting with # are
comments; write options as you would on the winws command line, quoting
paths that contain spaces.

The default is zapret's own default strategy, plus a Windows packet filter
(--wf-tcp / --wf-udp) for HTTP, HTTPS and QUIC.

To find a strategy that works on your network, run blockcheck from
zapret-win-bundle: https://github.com/bol-van/zapret-win-bundle
Copy the recommended winws options into winws.conf (keep the --wf-... line).

winws.conf is kept when you upgrade or uninstall Zapret.


Troubleshooting
---------------

  * "winws stopped right after starting": an option in winws.conf is
    wrong, or the WinDivert driver could not load. The message shows the
    last lines winws printed; the full output of the last run is in
        C:\ProgramData\Zapret\winws.log
    To watch winws live, switch zapret off, then run this in an
    administrator command prompt (Ctrl+C to stop):
        "C:\Program Files\Zapret\zapret-service.exe"
  * Some antivirus products flag WinDivert (the packet driver zapret uses).
    It is a signed, open-source driver; allow it if you trust this setup.
  * Only one WinDivert-based DPI tool (GoodbyeDPI, another zapret) should
    run at a time.
  * Check the service:  sc query zapret


Uninstall
---------

Settings > Apps > Zapret > Uninstall. This removes the app and the service.
winws.conf is left in C:\Program Files\Zapret; delete it by hand if you
don't need it.


Licenses
--------

This app: MIT (LICENSE.txt). winws and zapret: MIT, by bol-van
(winws\LICENSE-zapret.txt). WinDivert: LGPL v3 / GPL v2, by basil00.
