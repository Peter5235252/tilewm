#!/bin/sh
# tilewm-wslg: launch tilewm for WSLg using the X11 backend.
#
# Why: under WSLg's Weston, maximizing a wlroots Wayland-backend window
# sends a maximized configure the backend cannot satisfy, so the host
# disconnects us with an xdg_wm_base protocol error and the window dies.
# The X11 backend handles host resizes gracefully and survives maximize.
# On real hardware (DRM) or other Wayland/X11 sessions, run ./build/tilewm
# directly instead so backend autocreate can pick Wayland or DRM.
#
# Usage: ./run-wslg.sh [-- tilewm args...]   (args are passed through)

if [ -z "$DISPLAY" ]; then
    echo "tilewm-wslg: DISPLAY is not set; is WSLg running?" >&2
    exit 1
fi

BIN="$(dirname "$0")/build/tilewm"
if [ ! -x "$BIN" ]; then
    echo "tilewm-wslg: $BIN not found; build first:" >&2
    echo "  cmake -S . -B build -G Ninja && cmake --build build" >&2
    exit 1
fi

# Force backend autocreate to skip Wayland and fall back to X11 (:0).
unset WAYLAND_DISPLAY
export DISPLAY
exec "$BIN" "$@"
