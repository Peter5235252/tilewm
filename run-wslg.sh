#!/bin/sh
# tilewm-wslg: launch tilewm for WSLg using the X11 backend.
#
# Why X11 here: under WSLg's Weston, maximizing a wlroots Wayland-backend
# window sends a maximized configure the backend cannot satisfy, so the
# host disconnects us with an xdg_wm_base protocol error and the window
# dies. The X11 backend survives maximize and arbitrary resizes.
# On real hardware (DRM) or other Wayland/X11 sessions, run ./build/tilewm
# directly instead so backend autocreate can pick Wayland or DRM.
#
# WSLg quirk handled below: the wlroots X11 backend sets no WM_CLASS, and
# Weston's XWM ignores class-less windows (never forwarded to RAIL, so no
# Windows window appears). We tag our output windows and cycle their
# mapping so the window manager picks them up. Requires: xdotool.
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

# Tag output windows ("wlroots - X11-N") with a WM_CLASS and re-map them so
# Weston's XWM forwards them to RAIL. Runs in the background while the
# compositor starts; exits after all current outputs are tagged.
#
# Discovery uses xwininfo's window tree on purpose: xdotool search relies
# on _NET_CLIENT_LIST, which Weston's XWM does not maintain, so it never
# finds our windows here.
tag_outputs() {
    if ! command -v xdotool >/dev/null 2>&1 || ! command -v xwininfo >/dev/null 2>&1; then
        echo "tilewm-wslg: xdotool/xwininfo not found; windows may not appear." >&2
        echo "tilewm-wslg: install them: sudo dnf install xdotool xwininfo" >&2
        return 0
    fi
    n=0
    while [ "$n" -lt 150 ]; do
        ids=$(DISPLAY="$DISPLAY" xwininfo -root -tree 2>/dev/null | sed -n 's/^ *\(0x[0-9a-f][0-9a-f]*\) "wlroots - X11-[0-9][0-9]*".*/\1/p')
        if [ -n "$ids" ]; then
            for id in $ids; do
                xdotool set_window --class tilewm --classname tilewm "$id" 2>/dev/null
                xdotool windowunmap "$id" 2>/dev/null
                sleep 0.3
                xdotool windowmap "$id" 2>/dev/null
            done
            return 0
        fi
        n=$((n + 1))
        sleep 0.2
    done
    echo "tilewm-wslg: no output windows found; window may not appear." >&2
}

tag_outputs &

# Force backend autocreate to skip Wayland and fall back to X11 (:0).
unset WAYLAND_DISPLAY
export DISPLAY
exec "$BIN" "$@"
