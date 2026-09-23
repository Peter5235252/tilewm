# tilewm — a tiny tiling Wayland compositor in C++ (wlroots 0.20)

Built and tested on Fedora 44 under WSL2 / WSLg, but it should run on any
Linux with wlroots 0.20: nested under another Wayland/X11 session for
development, or on DRM/KMS on real hardware.

## Dependencies (Fedora 44)

```
sudo dnf install gcc gcc-c++ cmake ninja-build pkgconf-pkg-config git \
  wlroots-devel wayland-devel wayland-protocols-devel libxkbcommon-devel \
  libinput-devel pixman-devel libseat-devel mesa-libEGL-devel \
  mesa-libGLES-devel libdrm-devel systemd-devel \
  foot wayland-utils wlr-randr
```

## Build

```
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

## Run (nested inside WSLg)

WSLg remotes every Linux window separately (RAIL), so the way to get a
contained desktop feel is one big nested window with all clients inside it:

```
./run-wslg.sh
```

This forces the **X11 backend**, which is currently the durable choice under
WSLg: maximizing a wlroots **Wayland**-backend window makes the host send a
maximized configure the backend cannot satisfy, and the host disconnects us
with an `xdg_wm_base` protocol error. The X11 backend survives maximize and
arbitrary resizes. On real hardware or other sessions, run `./build/tilewm`
directly so backend autocreate can pick Wayland or DRM.

 tips:
- Maximize the tilewm window (`Win+Up`) and open clients *inside* it with
  `Alt+Return`; host-side terminals stay outside and only add clutter.
- Optionally move it to its own Windows virtual desktop (`Win+Tab` -> New
  desktop, drag it over, `Win+Ctrl+Left/Right` to flip).

It prints `WAYLAND_DISPLAY=wayland-1` (or similar). From another terminal:

```
WAYLAND_DISPLAY=wayland-1 foot
```

## Keybindings (Phase 2)

| Keys                | Action                        |
|---------------------|-------------------------------|
| `Alt+Return`        | spawn terminal (`foot`)       |
| `Alt+J` / `Alt+K`   | focus next / previous window  |
| `Alt+Space`         | toggle floating on focused    |
| `Alt+1` … `Alt+4`   | switch workspace              |
| `Alt+Shift+1` … `4` | move focused window + refocus |
| `Alt+Q`             | close focused window          |
| `Alt+Shift+E`       | quit the compositor           |
| click               | focus window                  |

## Roadmap

- Phase 1 (done): bring-up, scene rendering, floating xdg-shell views, focus.
- Phase 2 (here): master-stack tiling, focus cycling, floating toggle,
  4 workspaces.
- Phase 3: layer-shell bar support, XWayland, config file (gaps, mfact,
  nmaster, keybinds), multi-output polish.
