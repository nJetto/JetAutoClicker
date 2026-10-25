# JetAutoClicker

A small autoclicker for Linux written in C with a GTK3 interface.
Works on **Wayland** — including **niri**, sway and other wlroots compositors,
KDE Plasma (KWin) and GNOME — and on **X11**.

## Supported environments

| Session | Click method | High CPS | Extra permissions |
|---|---|---|---|
| niri 25.02+ | Wayland virtual pointer | yes | hotkey only |
| sway / wlroots with `wlr-virtual-pointer` | Wayland virtual pointer | yes | hotkey only |
| KDE Plasma 6 Wayland | Portal (RemoteDesktop) | yes | hotkey only |
| GNOME Wayland | Portal (RemoteDesktop) | yes | hotkey only |
| X11 | uinput | no (~30 CPS) | hotkey + `/dev/uinput` |

The **Wayland virtual pointer** and **Portal** methods inject events above
libinput, so they are not affected by button debouncing. The **uinput** method
is a kernel virtual mouse and is limited by libinput's ~25 ms button debounce.

## Features

- Three click methods, plus **Auto** mode:
  - **Wayland virtual pointer** — niri / sway / wlroots. Any CPS, no extra permissions.
  - **Portal** — xdg-desktop-portal RemoteDesktop. Any CPS on KWin and GNOME.
  - **uinput** — kernel virtual mouse. Low CPS only (see below).
- Choose the exact **clicks per second** (0.1 – **1000 CPS**), or set the **interval in ms** instead
- Left / right / middle button
- **Custom global hotkey**: any single key or a mouse side button. It works even while Ctrl/Alt/Shift/Meta are held (e.g. sprinting with Ctrl in a game)
- Toggle mode (press to start/stop) or hold mode (click while the key is held)
- Live "measured CPS" readout, settings saved to `~/.config/jetautoclicker/config.ini`

## How it works

- **Wayland virtual pointer (niri, sway, ...):** clicks are sent through the
  `zwlr_virtual_pointer_manager_v1` protocol. The compositor receives them above
  libinput, so high CPS works. This backend needs no device permissions and no
  portal dialog. niri supports the protocol since 25.02 (current Arch packages
  have it). Auto mode selects this backend when it is available.
- **Portal method (KWin / GNOME):** clicks are sent through the `xdg-desktop-portal`
  RemoteDesktop interface. The compositor receives them above libinput, so high CPS
  works. On first start a permission dialog appears; the permission is remembered
  (restore token in `~/.config/jetautoclicker/portal_token`).
- **uinput method (fallback):** a kernel virtual mouse. Limited to about 30 CPS,
  because libinput (used by niri, KWin, GNOME, ...) debounces buttons: if a button
  changes state within ~25 ms it treats that as hardware bounce, delivers only the
  first press and delays the release, which looks like a held-down button. This
  cannot be worked around from a uinput device.
- Wayland apps cannot grab global keys, so the hotkey is read directly from
  `/dev/input/event*`. The hotkey is **not** swallowed: the focused application
  still receives that key press.
- Click timing uses absolute `CLOCK_MONOTONIC` deadlines on a dedicated thread; the
  engine measures ~999 CPS at a 1000 CPS target in the built-in test (without the
  compositor's own latency).

### niri notes

- Use the default **Auto** click method (or **Wayland virtual pointer**). The
  GNOME portal backend that niri uses for screencasting does **not** implement
  RemoteDesktop, so the Portal method is not available on a plain niri session.
- The global hotkey still uses evdev and therefore needs the `input` group (see
  [Permissions](#permissions)). Pick a key that is not one of your niri binds if
  you do not want niri to react to it as well.
- To keep the small window floating, add this to `~/.config/niri/config.kdl`:

  ```kdl
  window-rule {
      match app-id="jetautoclicker"
      open-floating true
  }
  ```

## Requirements

Runtime:

- GTK3 and GLib/GIO;
- `libwayland-client` for the niri/sway backend;
- `xdg-desktop-portal` plus a RemoteDesktop-capable backend for KDE/GNOME;
- read access to `/dev/input/event*` for the global hotkey (normally the
  `input` group);
- optionally the kernel `uinput` module and write access to `/dev/uinput` for
  the fallback backend.

Build:

- `gcc` / `make` / `pkg-config` (`pkgconf` on Arch);
- GTK3 and GIO development headers;
- `wayland-client` headers and `wayland-scanner` (part of the `wayland`
  package on Arch, `libwayland-dev` on Debian/Ubuntu).

## Build

```
# Arch Linux
sudo pacman -S --needed base-devel gtk3 wayland pkgconf

# Debian / Ubuntu / Mint
sudo apt install build-essential pkg-config libgtk-3-dev libwayland-dev

# Fedora
sudo dnf install gcc make pkgconf-pkg-config gtk3-devel wayland-devel

make
./jetautoclicker
```

`wayland-scanner` generates the wlr-virtual-pointer protocol code from the
bundled XML. The build does not need the separate `wlr-protocols` package.
The application version is compiled from the `VERSION` variable in the
`Makefile`; check it with `./jetautoclicker --version`.

Run the checks:

```
make test          # click timing, no GUI, writes to /dev/null
make test-wayland  # checks that the running compositor supports wlr-virtual-pointer
```

`make test-wayland` only connects to the compositor and prints the backend
status; it does not inject any clicks.

## Install on Arch Linux

### pacman package (recommended)

The repository contains a `PKGBUILD` for a local package:

```
make dist
cp build/jetautoclicker-1.1.0.tar.gz .
makepkg -si
```

This installs the binary to `/usr/bin`, the `.desktop` entry and icon, the udev
rule and the `uinput` module-load file. `makepkg` also prints the post-install
note about the `input` group.

### Without a package

```
sudo make install PREFIX=/usr
```

## Install as a .deb (Debian / Ubuntu)

```
make deb
sudo apt install ./build/jetautoclicker_1.1.0_amd64.deb
```

The package installs the binary to `/usr/bin`, a `.desktop` entry, an icon, the
udev rule and the `uinput` module-load file, so the app appears in the
application launcher (Utilities) and can be started without a terminal. Add
yourself to the `input` group as described under
[Permissions](#permissions) for the global hotkey.

## Permissions

The **Wayland virtual pointer** backend needs no permissions. The other two
features do:

### Global hotkey (all distributions)

The hotkey is read from `/dev/input/event*`, so your user must be able to read
input devices. On Arch, Debian, Fedora and most other distributions this means
membership in the `input` group:

```
sudo usermod -aG input $USER
```

Then **log out and back in**. Check with:

```
id
```

Security note: membership in the `input` group lets a program read all keyboard
input. This app only looks at key events to match your hotkey, but you should be
aware of what the group allows.

### uinput fallback

Only needed for the **uinput** click method. From the source tree:

```
sudo make setup-permissions
```

The target installs the udev rule, loads the `uinput` module at boot, adds you
to the `input` group (for the hotkey) and reloads udev. It creates:

- `/etc/udev/rules.d/99-jetautoclicker.rules`
- `/etc/modules-load.d/uinput.conf`

**Log out and back in** afterwards. Manual equivalent:

```
sudo cp 99-jetautoclicker.rules /etc/udev/rules.d/
echo uinput | sudo tee /etc/modules-load.d/uinput.conf
sudo modprobe uinput
sudo udevadm control --reload
sudo udevadm trigger
```

The Arch package and `make install PREFIX=/usr` already install the udev rule
and module-load file; after installing a package you only need the group
membership for the hotkey.

## Configuration

Settings are stored in `~/.config/jetautoclicker/config.ini`:

```ini
[main]
rate_mode=0        # 0 = clicks per second, 1 = interval in milliseconds
rate_value=10      # value for the selected mode
button=0           # 0 = left, 1 = right, 2 = middle
hold_mode=0        # 0 = toggle, 1 = hold while the hotkey is pressed
backend_mode=0     # 0 = Auto, 1 = Portal, 2 = Wayland virtual pointer, 3 = uinput
backend=0          # legacy key from 1.0.x; ignored when backend_mode exists
hotkey=64          # Linux evdev key code (F6 by default)
```

The file is written when the window is closed. You can edit it by hand while
the app is not running, but using the UI is safer for the hotkey. The portal
restore token is kept separately in `~/.config/jetautoclicker/portal_token`.

Old 1.0.x configs are migrated automatically: the old portal default becomes
**Auto**, so niri/sway switch to the Wayland virtual pointer while KDE/GNOME
keep using the portal.

## Usage

1. Pick **CPS** or **Interval** mode and enter the value.
2. Leave **Click method** on **Auto** unless you want to force one. Auto picks
   the Wayland virtual pointer on niri/sway, the portal on KDE/GNOME and uinput
   on X11 or when nothing better is available.
3. Click the hotkey button, then press the single key you want. Modifier keys are
   ignored, so the hotkey still works while they are held. `Esc` cancels.
4. Put the cursor where you want to click and press the hotkey (or use **Start**).

If the global hotkey is disabled, the status line says that no readable keyboard
was found — check the `input` group membership and log in again.

## Notes on very high CPS

- The "Measured" value counts clicks the program emitted. Some games and apps
  process input at a fixed rate or cap click speed, so they may register fewer
  clicks than 1000/s.
- With the Portal method every click is two D-Bus messages going through two
  daemons (xdg-desktop-portal and the desktop backend), so the real ceiling
  depends on your machine. The Wayland virtual pointer is a direct Wayland
  socket write and is lighter.
- Each click presses the button for half the period (max 20 ms), so press and
  release are always separate events, even at 1000 CPS.

## Troubleshooting

- **"compositor does not support wlr-virtual-pointer"** — niri older than 25.02,
  or the app is sandboxed (for example a Flatpak) and the compositor hides the
  protocol. Update niri or select **uinput**. `make test-wayland` prints the
  detected status.
- **The Portal method fails on niri** — expected. niri's GNOME portal backend is
  for screencasting and does not implement RemoteDesktop. Use **Auto** or
  **Wayland virtual pointer**.
- **"No readable keyboard found"** — the evdev hotkey has no permission. Run
  `sudo usermod -aG input $USER`, then log out and back in; check with `id`.
- **The window is tiled** — add the niri `window-rule` shown above.
- **uinput is slow or the button looks held** — this is libinput debouncing;
  use Auto/Wayland/Portal instead of uinput.
- **Clicks are sent but a game ignores them** — some games read input at a
  fixed rate or use raw input. The "Measured" line shows what the app sent, not
  what the game accepted.

## Uninstall

If installed from the Arch package:

```
sudo pacman -Rns jetautoclicker
```

If installed manually with `make install`:

```
sudo make uninstall PREFIX=/usr
```

Optionally remove the configuration and portal token:

```
rm -rf ~/.config/jetautoclicker
```

You can also remove your user from the `input` group if no other tool needs it:

```
sudo gpasswd -d $USER input
```

## Files

| File | Purpose |
|---|---|
| `main.c` | GTK3 interface, config and Auto backend selection |
| `engine.c / engine.h` | click scheduler, uinput backend, evdev hotkey listener (no GTK dependency) |
| `portal.c / portal.h` | xdg-desktop-portal RemoteDesktop backend (GIO) |
| `wayland.c / wayland.h` | wlr-virtual-pointer backend for niri/sway (libwayland-client) |
| `protocols/wlr-virtual-pointer-unstable-v1.xml` | bundled protocol description (MIT, wlr-protocols) |
| `test_engine.c` | timing smoke test (`make test`) |
| `test_wayland.c` | compositor protocol check (`make test-wayland`) |
| `PKGBUILD`, `jetautoclicker.install` | Arch Linux package |
| `CHANGELOG.md` | release notes |
| `99-jetautoclicker.rules`, `99-jetautoclicker.modules` | udev rule and `uinput` module-load file |
| `jetautoclicker.desktop`, `jetautoclicker.svg` | application menu entry and icon |
| `debian-control.in`, `debian-postinst`, `debian-postrm` | metadata and scripts for `make deb` |
