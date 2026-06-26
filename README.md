# Scrotocol

A native GTK4/C++ desktop front-end for [`scrot`](https://github.com/resurrecting-open-source-projects/scrot), the command-line screen capture utility. Scrotocol adds a GUI on top of `scrot`'s capture engine: capture modes with a delay countdown, an in-app crop tool, clipboard/file export, a persistent on-disk history with thumbnails, and a system tray icon for background use.

## Features

- **Capture modes** — Full Screen, Active Window, and an interactive Region select, each driven by `scrot` under the hood.
- **Delay countdown** — an optional 0-10s delay (configurable via the header bar spinner) shown as an in-app countdown before capture fires.
- **Crop** — click-drag a selection directly on the captured preview, then *Apply Crop* / *Reset* to commit or discard it.
- **Clipboard copy** — pushes the current working image onto the system clipboard as a `GdkTexture` (shows up as `image/png`, `image/tiff`, etc. to other apps).
- **Save As** — native `GtkFileDialog` save dialog, pre-filled with a timestamped filename and the history folder as the starting location.
- **History panel** — every capture is auto-saved to a configurable folder (default `~/Pictures/Scrotocol`) and listed as thumbnails in a sidebar; click a thumbnail to reload it into the preview, or delete it.
- **Tray icon** — a `org.kde.StatusNotifierItem` + `com.canonical.dbusmenu` implementation over raw GDBus (see [Tray icon details](#tray-icon-details) below). Left-click toggles the main window. Right-click opens a context menu: Full Screen, Region, Window, Timer (with current delay value), Settings/Show, and Quit. Closing the window (the titlebar X) quits normally — the tray is purely an opt-in "minimize to tray" affordance via the down-arrow header button.

## Dependencies

| Dependency | Notes |
|---|---|
| `scrot` | The actual screenshot engine; must be on `PATH`. Developed against scrot 2.0.0. |
| GTK4 | 4.10+ required for `GtkFileDialog` and `GtkUriLauncher`. |
| `gio-unix-2.0` | For `GSubprocess` (launching `scrot`) and `GDBus` (the tray icon). |
| CMake 3.16+ | Build system. |
| A C++20 compiler | GCC or Clang. |

On Arch/Archcraft:

```sh
sudo pacman -S scrot gtk4 cmake gcc pkgconf
```

## Building

```sh
cmake -S . -B build
cmake --build build -j$(nproc)
```

This produces `build/scrotocol`. `CMAKE_EXPORT_COMPILE_COMMANDS` is enabled, so a `compile_commands.json` is generated under `build/` for clangd/IDE tooling (symlink it to the project root if your editor expects it there: `ln -s build/compile_commands.json .`).

## Running

```sh
./build/scrotocol
```

To install system-wide (binary + `.desktop` entry):

```sh
sudo cmake --install build
```

## Configuration

Settings are stored in `$XDG_CONFIG_HOME/scrotocol/config.ini` (typically `~/.config/scrotocol/config.ini`), written automatically the first time a setting changes:

```ini
[scrotocol]
history_dir=/home/you/Pictures/Scrotocol
default_delay=0
```

- `history_dir` — where auto-saved captures and their thumbnails live. Change it by editing the file directly (no in-app picker yet) and restarting the app.
- `default_delay` — remembers the last value set in the header bar's delay spinner.

## Project layout

```
src/
  main.cpp          GApplication/GtkApplication entry point
  MainWindow.*       Builds the UI and wires every feature together
  Capture.*          Async scrot invocation (GSubprocess) per capture mode
  CropOverlay.*       GtkOverlay + GtkDrawingArea widget for the crop-selection UI
  HistoryStore.*      Scans/writes the history folder, generates thumbnails
  TrayIcon.*           Minimal StatusNotifierItem service over GDBus
  Config.*             GKeyFile-backed settings (history dir, default delay)
data/
  scrotocol.desktop    Freedesktop application launcher entry
```

## How capture works

Clicking a capture button hides the main window, waits ~150ms for the window manager to settle, then runs `scrot` with mode-specific flags and a temp output path:

| Mode | scrot flags |
|---|---|
| Full Screen | *(none — plain capture)* |
| Active Window | `-u` (focused window) `-b` (include window border) |
| Region | `-s` (interactive select) `-f` (freeze screen during selection) |

All modes add `-o` (overwrite) and `-z` (silent/no beep). The delay countdown is handled entirely in-app (so it's visible without a terminal attached) — by the time `scrot` runs, no extra delay flag is needed. On success the image is loaded into the crop preview and auto-saved into the history folder; if the user cancels an interactive region selection (Escape), `scrot` exits without producing a file and Scrotocol treats that as a silent no-op.

## Tray icon details

GTK4 removed `GtkStatusIcon`, and `libappindicator`/`libayatana-appindicator` link GTK3 — running GTK3 and GTK4 in the same process risks symbol clashes, since both export many identically-named C symbols. To avoid that, the tray icon is a from-scratch, dependency-free implementation of the [StatusNotifierItem](https://www.freedesktop.org/wiki/Specifications/StatusNotifierItem/) spec over `GDBus`:

- Registers a well-known bus name `org.kde.StatusNotifierItem-<pid>-1` and an SNI object at `/StatusNotifierItem`.
- Calls `RegisterStatusNotifierItem` on `org.kde.StatusNotifierWatcher`.
- Implements `Activate`/`SecondaryActivate` (toggle window visibility).
- Registers a `com.canonical.dbusmenu` object at `/MenuBar` (the `Menu` property on the SNI points here). Right-click opens a popup menu built from this layout by the tray host or bridge (snixembed/libdbusmenu-gtk3 on tint2). Menu item clicks arrive as `EventGroup` calls from snixembed (not individual `Event` calls -- a non-obvious protocol detail).
- At startup, writes a one-line GTK3 CSS rule to `~/.config/gtk-3.0/gtk.css` (idempotent) to ensure menu separators are visible on themes that use the same color for menu background and separator (e.g. Arc-Dark). Takes effect on the next snixembed start.

This requires a StatusNotifierWatcher/host to be running. Most modern desktop panels (KDE Plasma, GNOME Shell with an extension, waybar) provide one natively. On lighter setups (Openbox, etc.) with an XEmbed-only tray like `tint2` or `stalonetray`, run [`snixembed`](https://git.sr.ht/~steef/snixembed) alongside it to bridge StatusNotifierItem icons into the legacy tray — this is exactly the setup verified during development (`tint2` + `snixembed` on Openbox).

Note: **Plank does not work as a tray host.** It's a launcher/window dock with no tray or systray module, so the icon will never appear there no matter what's bridging it — `snixembed` + `tint2` (or another systray-capable panel) is required even if Plank is also running.

## Known limitations

- `gdk_texture_new_for_pixbuf` is deprecated as of GTK 4.20 with no direct replacement yet; it's still fully functional in GTK 4.22 and is used throughout since the app's image pipeline is built around `GdkPixbuf`. Revisit if/when GTK ships a real replacement.
- No keyboard shortcuts or a preferences dialog yet — delay and history folder are the only configurable settings, and the history folder can currently only be changed by hand-editing `config.ini`.
- Tray right-click requires a StatusNotifierWatcher host that supports DBusMenu (snixembed + tint2 on Openbox). On XEmbed-only trays without snixembed, the icon won't appear at all.
