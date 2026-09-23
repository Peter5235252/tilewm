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

```
./build/tilewm
```

It prints `WAYLAND_DISPLAY=wayland-1` (or similar). From another terminal:

```
WAYLAND_DISPLAY=wayland-1 foot
```

## Keybindings (Phase 1)

| Keys            | Action                        |
|-----------------|-------------------------------|
| `Alt+Return`    | spawn terminal (`foot`)       |
| `Alt+Q`         | close focused window          |
| `Alt+Shift+E`   | quit the compositor           |
| click           | focus window                  |

## Roadmap

- Phase 1 (here): bring-up, scene rendering, floating xdg-shell views, focus.
- Phase 2: master-stack tiling from `src/tiling.hpp`, workspaces, floating toggle.
- Phase 3: layer-shell bar support, XWayland, config file, multi-output polish.
