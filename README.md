# JetAutoClicker

A small autoclicker for Linux written in C with a GTK3 interface.
Works on **Wayland (KWin / KDE Plasma)** and X11, kernel 6.12+ (anything with `uinput`).

## Features

- Two click methods: Portal (any CPS on Wayland) and uinput (low CPS)
- Choose the exact **clicks per second** (0.1 – **1000 CPS**), or set the **interval in ms** instead
- Left / right / middle button
- **Custom global hotkey**: any single key or a mouse side button. It works even while Ctrl/Alt/Shift/Meta are held (e.g. sprinting with Ctrl in a game)
- Toggle mode (press to start/stop) or hold mode (click while the key is held)
- Live "measured CPS" readout, settings saved to `~/.config/jetautoclicker/config.ini`

## How it works

- **Portal method (default):** clicks are sent through the `xdg-desktop-portal` RemoteDesktop
  interface, which KWin (Plasma 6) and GNOME implement. The compositor receives them above
  libinput, so high CPS works. On first start a permission dialog appears; the permission is
  remembered (restore token in `~/.config/jetautoclicker/portal_token`).
- **uinput method (fallback):** a kernel virtual mouse. Limited to about 30 CPS, because libinput
  (used by KWin) debounces buttons: if a button changes state within ~25 ms it treats that as
  hardware bounce, delivers only the first press and delays the release, which looks like a
  held-down button. This cannot be worked around from a uinput device.
- Wayland apps cannot grab global keys, so the hotkey is read directly from `/dev/input/event*`.
  The hotkey is **not** swallowed: the focused application still receives that key press.
- Click timing uses absolute `CLOCK_MONOTONIC` deadlines on a dedicated thread; the engine
  measures ~999 CPS at a 1000 CPS target in the built-in test (without the portal's own latency).

## Build

```
# Debian / Ubuntu / Mint
sudo apt install build-essential pkg-config libgtk-3-dev
# Fedora:  sudo dnf install gcc make pkgconf-pkg-config gtk3-devel
# Arch:    sudo pacman -S base-devel gtk3

make
./jetautoclicker
```

### Install as a .deb (adds JetAutoClicker to the KDE application menu)

```
make deb
sudo apt install ./build/jetautoclicker_1.0.1_amd64.deb
```

The package installs the binary to `/usr/bin`, a `.desktop` entry and an icon, so the app
appears in the application launcher (Utilities) and can be started without a terminal.
Without a package, `sudo make install PREFIX=/usr` does the same.

## One-time permissions

Read access to `/dev/input/event*` is needed for the global hotkey, and write access to
`/dev/uinput` only for the uinput fallback:

```
sudo make setup-permissions
```

(installs a udev rule, loads the `uinput` module at boot, adds you to the `input` group).
**Log out and back in** afterwards. Manual equivalent:

```
sudo cp 99-jetautoclicker.rules /etc/udev/rules.d/
echo uinput | sudo tee /etc/modules-load.d/uinput.conf
sudo modprobe uinput
sudo usermod -aG input $USER
```

Security note: membership in the `input` group lets a program read all keyboard input. This
app only looks at key events to match your hotkey, but you should be aware of what the group allows.

## Usage

1. Pick **CPS** or **Interval** mode and enter the value.
2. Click the hotkey button, then press the single key you want. Modifier keys are ignored, so the hotkey still works while they are held. `Esc` cancels.
3. Put the cursor where you want to click and press the hotkey (or use **Start**).

## Notes on very high CPS

- The "Measured" value counts clicks the program emitted. Some games and apps process input
  at a fixed rate or cap click speed, so they may register fewer clicks than 1000/s.
- With the Portal method every click is two D-Bus messages going through two daemons
  (xdg-desktop-portal and the KDE backend), so the real ceiling depends on your machine.
  Watch the "Measured" line; it counts what the app sent.
- Each click presses the button for half the period (max 20 ms), so press and release are
  always separate events, even at 1000 CPS.
  
## Uninstall

Use the case that matches how you installed it.

**Installed from the `.deb` package:**

```
sudo apt remove jetautoclicker
```

**Installed with `sudo make install`:** there is no uninstall target, so remove the files by hand
(if you used a different `PREFIX`, replace `/usr` with it):

```
sudo rm /usr/bin/jetautoclicker
sudo rm /usr/share/applications/jetautoclicker.desktop
sudo rm /usr/share/icons/hicolor/scalable/apps/jetautoclicker.svg
```

**Only built and run from the source folder:** nothing is installed, just delete the folder.

### Remove settings

```
rm -rf ~/.config/jetautoclicker
```

This deletes the saved settings and the portal permission token.

### Revert the permissions (only if you ran `make setup-permissions`)

```
sudo rm /etc/udev/rules.d/99-jetautoclicker.rules
sudo rm /etc/modules-load.d/uinput.conf
sudo udevadm control --reload
sudo gpasswd -d "$USER" input
```

Only run the last command if no other program needs the `input` group. Log out and back in
afterwards. The `uinput` module is unloaded on the next reboot (or run `sudo modprobe -r uinput`).

## Files

| File | Purpose |
|---|---|
| `main.c` | GTK3 interface and config |
| `engine.c / engine.h` | click scheduler, uinput backend, evdev hotkey listener (no GTK dependency) |
| `portal.c / portal.h` | xdg-desktop-portal RemoteDesktop backend (GIO) |
| `test_engine.c` | timing smoke test (`make test`) |
| `CHANGELOG.md` | release notes |
| `99-jetautoclicker.rules` | udev rule for `/dev/uinput` |
| `jetautoclicker.desktop`, `jetautoclicker.svg` | application menu entry and icon |
| `debian-control.in`, `debian-postinst`, `debian-postrm` | metadata and scripts for `make deb` |
