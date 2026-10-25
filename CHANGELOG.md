# Changelog

All notable changes to JetAutoClicker are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [1.1.0] - 2026-10-25

### Added
- **Wayland virtual pointer backend** (`zwlr_virtual_pointer_manager_v1`).
  Works on **niri**, sway and other wlroots compositors, needs no device
  permissions and supports high CPS because events bypass libinput.
- **Auto** click method: picks the virtual pointer on niri/sway, the portal on
  KDE/GNOME, and uinput on X11 or as a fallback.
- Arch Linux `PKGBUILD` and install script, plus `make dist` for a local
  package build.
- `make test-wayland` to check whether the running compositor supports the
  virtual pointer protocol.
- `--version` command-line flag; the version is taken from the `VERSION`
  variable in the `Makefile`.
- niri documentation: portal limitation, floating window rule and permissions.

### Changed
- The "Click method" list now contains Auto, Portal, Wayland virtual pointer
  and uinput. Existing configs are migrated automatically: the old default
  (portal) becomes Auto, so niri/sway switch to the working virtual pointer,
  while KDE/GNOME still use the portal. `backend_mode` is written next to the
  old `backend` key.
- The portal is only initialized when the selected method needs it, so niri
  users no longer get a failing portal connection at startup.
- The build now depends on `wayland-client` and `wayland-scanner`; the protocol
  XML is bundled, so `wlr-protocols` is not required.
- `make install` installs the udev rule and the `uinput` module-load file.
- The `.deb` post-install script reloads udev and loads `uinput`, so the
  fallback backend works without a reboot.

### Fixed
- Clicking under niri: the RemoteDesktop portal is not implemented by niri, so
  the previous portal-only default could not click at all.

## [1.0.1] - 2026-10-18

### Fixed
- The global hotkey did not fire while a modifier key was held down. For example, with the
  hotkey set to `R`, pressing it while holding `Ctrl` (sprinting in Minecraft) did nothing.

### Changed
- The hotkey is now always a **single key** (or a mouse side button). Modifier keys
  (`Ctrl`, `Alt`, `Shift`, `Meta`) are ignored, so the hotkey works no matter which
  modifiers are held at that moment.
- When recording a hotkey, pressing a combination such as `Ctrl+R` now records just `R`.
  Pressing a modifier key on its own is ignored, and `Esc` always cancels the recording.
- Hold mode now simply follows the hotkey: pressing it starts clicking, releasing it stops.
- README updated to describe the single-key hotkey.

### Removed
- Modifier combinations as hotkeys (e.g. `Ctrl+Alt+F6`).
- The `hotkey_mods` entry in `~/.config/jetautoclicker/config.ini`. It is no longer read or
  written. Existing configs keep working: a saved `Ctrl+R` simply becomes `R`, and the
  leftover `hotkey_mods` line can be deleted by hand.

### Internal
- Engine API: the `MOD_*` constants and the `mods` parameter were removed from
  `engine_set_hotkey()`, `engine_get_hotkey()`, `engine_hotkey_name()` and the
  `EngineCaptureFn` callback.

## [1.0.0]

### Added
- Initial release: GTK3 autoclicker for Linux (Wayland / X11).
- Two click methods: xdg-desktop-portal RemoteDesktop (any CPS on Wayland) and uinput.
- Click rate from 0.1 to 1000 CPS, set either as CPS or as an interval in ms.
- Left / right / middle mouse button.
- Custom global hotkey read from evdev, with toggle and hold modes.
- Live "measured CPS" readout and settings saved to `~/.config/jetautoclicker/config.ini`.
- `.deb` packaging, udev rule and KDE/GNOME application menu entry.
