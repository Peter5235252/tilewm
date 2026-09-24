#!/usr/bin/env bash
# tilewm installer: Arch Linux and Fedora only (for now).
#
#   ./install.sh [--yes] [--no-config] [--prefix DIR] [--source DIR]
#
# What it does:
#   1. Refuses to run as root (sudo is used only for the package step).
#   2. Detects Arch vs Fedora via /etc/os-release and installs every
#      dependency from official repositories (no AUR, no COPR).
#   3. Clones (or fast-forward updates) tilewm, configures, builds, tests.
#   4. Installs example init.lua, foot.ini and wallpaper into ~/.config,
#      backing up anything already there.
#   5. Prints a tailored "what now" card (bare metal vs WSLg).
#
# Interface: gum menus/spinners when available, plain prompts otherwise.
# Non-interactive: ./install.sh --yes

set -euo pipefail

REPO_URL="https://github.com/Peter5235252/tilewm.git"
ASSUME_YES=0
DO_CONFIG=1
PREFIX="$HOME"
SOURCE_DIR=""

usage() {
    cat <<EOF
Usage: ./install.sh [--yes] [--no-config] [--prefix DIR] [--source DIR]

  --yes         answer yes to every prompt (automation friendly)
  --no-config   skip installing example configs into ~/.config
  --prefix DIR  clone tilewm into DIR instead of \$HOME
  --source DIR  use an existing checkout at DIR instead of cloning
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --yes) ASSUME_YES=1 ;;
        --no-config) DO_CONFIG=0 ;;
        --prefix) PREFIX="${2:?--prefix needs a directory}"; shift ;;
        --source) SOURCE_DIR="${2:?--source needs a directory}"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "install.sh: unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

log()  { printf '==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 1. Safety: never as root, sudo up front.
# ---------------------------------------------------------------------------
[ "$(id -u)" -ne 0 ] || die "do not run as root; sudo is requested only for packages."

# Fail fast when sudo would need a password we cannot provide: a hanging
# password prompt with no terminal looks exactly like a frozen installer.
if ! sudo -n true 2>/dev/null; then
    if [ ! -t 0 ]; then
        die "sudo needs a password but there is no terminal to ask on. Authenticate first (sudo -v) in a real terminal, then re-run."
    fi
    warn "sudo authentication required for the package step ..."
    sudo -v || die "sudo authentication failed."
fi

# ---------------------------------------------------------------------------
# 2. Distro detection: Arch family or Fedora, nothing else (for now).
# ---------------------------------------------------------------------------
DISTRO=""
if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    case "${ID:-} ${ID_LIKE:-}" in
        *arch*)  DISTRO="arch" ;;
        *fedora*) DISTRO="fedora" ;;
    esac
fi
[ -n "$DISTRO" ] || die "unsupported distro (ID=${ID:-unknown}): tilewm supports Arch Linux and Fedora only, for now."

ARCH_DEPS="base-devel cmake ninja pkgconf git wlroots0.20 wayland wayland-protocols libxkbcommon libinput libseat mesa libdrm lua libjpeg-turbo libpng foot gum xdotool xwininfo"
FEDORA_DEPS="gcc gcc-c++ cmake ninja-build pkgconf-pkg-config git wlroots-devel wayland-devel wayland-protocols-devel libxkbcommon-devel libinput-devel pixman-devel libseat-devel mesa-libEGL-devel mesa-libGLES-devel libdrm-devel systemd-devel lua-devel libjpeg-turbo-devel libpng-devel foot gum xdotool xwininfo"

# ---------------------------------------------------------------------------
# 3. TUI: gum when available, plain prompts otherwise.
# ---------------------------------------------------------------------------
USE_GUM=0
if command -v gum >/dev/null 2>&1; then
    USE_GUM=1
fi

confirm() {
    # confirm "Question" -> exit 0 on yes
    if [ "$ASSUME_YES" -eq 1 ]; then
        return 0
    fi
    if [ "$USE_GUM" -eq 1 ]; then
        gum confirm "$1"
    else
        printf '%s [y/N] ' "$1"
        read -r answer
        [ "$answer" = "y" ] || [ "$answer" = "Y" ]
    fi
}

choose_mode() {
    # prints: all | deps | build | config
    if [ "$ASSUME_YES" -eq 1 ]; then
        echo "all"
        return 0
    fi
    if [ "$USE_GUM" -eq 1 ]; then
        gum choose --header "What should the installer do?" \
            "Install everything" \
            "Dependencies only" \
            "Build and test only" \
            "Deploy example configs only" | awk '{
                if ($0 ~ /Dependencies/) print "deps";
                else if ($0 ~ /Build/) print "build";
                else if ($0 ~ /configs/) print "config";
                else print "all" }'
    else
        echo "1) Install everything"
        echo "2) Dependencies only"
        echo "3) Build and test only"
        echo "4) Deploy example configs only"
        printf 'Choice [1-4] (default 1): '
        read -r choice
        case "${choice:-1}" in
            2) echo "deps" ;;
            3) echo "build" ;;
            4) echo "config" ;;
            *) echo "all" ;;
        esac
    fi
}

run_step() {
    # run_step "Title" command...
    if [ "$USE_GUM" -eq 1 ]; then
        gum spin --spinner dot --title "$1" -- "${@:2}"
    else
        log "$1 ..."
        "${@:2}"
    fi
}

# ---------------------------------------------------------------------------
# 4. Plan.
# ---------------------------------------------------------------------------
MODE="$(choose_mode)"
log "distro: $DISTRO, mode: $MODE"

if [ "$MODE" != "build" ] && [ "$MODE" != "config" ]; then
    confirm "Install system packages with sudo?" || die "aborted."
fi

# ---------------------------------------------------------------------------
# 5. Dependencies (+ gum bootstrap for the rest of this run).
# ---------------------------------------------------------------------------
install_deps() {
    if [ "$DISTRO" = "arch" ]; then
        # shellcheck disable=SC2086
        sudo pacman -S --needed --noconfirm $ARCH_DEPS
    else
        # shellcheck disable=SC2086
        sudo dnf install -y $FEDORA_DEPS
    fi
    # A distro upgrade renames versioned packages (e.g. wlroots0.20);
    # fail loudly here instead of with a cryptic cmake error later.
    pkg-config --exists wlroots-0.20 \
        || die "wlroots-0.20 not found after install (renamed upstream? please report)."
    command -v gum >/dev/null 2>&1 && USE_GUM=1
}

# ---------------------------------------------------------------------------
# 6. Source: clone or fast-forward.
# ---------------------------------------------------------------------------
fetch_source() {
    if [ -n "$SOURCE_DIR" ]; then
        [ -d "$SOURCE_DIR/.git" ] || die "--source $SOURCE_DIR is not a git checkout."
        DEST="$SOURCE_DIR"
    else
        DEST="$PREFIX/tilewm"
        if [ -d "$DEST/.git" ]; then
            log "updating existing checkout at $DEST ..."
            git -C "$DEST" pull --ff-only \
                || die "$DEST has local changes; stash or move them first."
        else
            log "cloning into $DEST ..."
            git clone "$REPO_URL" "$DEST"
        fi
    fi
    printf '%s\n' "$DEST"
}

# ---------------------------------------------------------------------------
# 7. Build + test.
# ---------------------------------------------------------------------------
build_all() {
    log "configuring + building in $DEST ..."
    cmake -S "$DEST" -B "$DEST/build" -G Ninja
    cmake --build "$DEST/build"
    log "running tests ..."
    ctest --test-dir "$DEST/build" --output-on-failure
}

# ---------------------------------------------------------------------------
# 8. Example configs (with backups, never silent overwrites).
# ---------------------------------------------------------------------------
install_file() {
    # install_file <repo-relative-src> <dest-path>
    src="$DEST/$1"
    dest="$2"
    [ -f "$src" ] || die "missing $src (broken checkout?)."
    if [ -f "$dest" ] && ! cmp -s "$src" "$dest"; then
        backup="$dest.bak-$(date +%Y%m%d-%H%M%S)"
        log "backing up $dest -> $backup"
        cp "$dest" "$backup"
    fi
    mkdir -p "$(dirname "$dest")"
    cp "$src" "$dest"
    log "installed $dest"
}

deploy_configs() {
    install_file "examples/init.lua" "$HOME/.config/tilewm/init.lua"
    install_file "examples/foot.ini" "$HOME/.config/foot/foot.ini"
    install_file "assets/wallpaper.jpg" "$HOME/.config/tilewm/wallpaper.jpg"
}

# ---------------------------------------------------------------------------
# 9. Run it.
# ---------------------------------------------------------------------------
DEST=""
case "$MODE" in
    deps)   install_deps ;;
    build)  DEST="$(fetch_source)"; build_all ;;
    config) DEST="$(fetch_source)"; [ "$DO_CONFIG" -eq 1 ] && deploy_configs ;;
    all)
        install_deps
        DEST="$(fetch_source)"
        build_all
        [ "$DO_CONFIG" -eq 1 ] && deploy_configs
        ;;
esac

# ---------------------------------------------------------------------------
# 10. What-now card.
# ---------------------------------------------------------------------------
if [ "$MODE" = "all" ] || [ "$MODE" = "build" ]; then
    BIN="$DEST/build/tilewm"
    cat <<EOF

tilewm is ready: $BIN
  Run it:  ./run-wslg.sh          (inside WSLg: X11 backend, maximize freely)
           ./build/tilewm         (bare metal TTY or nested Wayland session)
  Test:    WAYLAND_DISPLAY=wayland-N foot
  Config:  ~/.config/tilewm/init.lua   (Alt+Shift+R reloads it live)

Keybindings: Alt+Return terminal | Alt+J/K focus | Alt+Space float |
  Alt+1..4 workspaces | Alt+Shift+1..4 move | Alt+Q close | Alt+Shift+E quit

Notes: on bare metal, launch from a TTY (Ctrl+Alt+F3) so the DRM backend
is picked; a normal TTY login provides the needed session permissions.
Under WSLg, keep clients inside the tilewm window and give it a virtual
desktop (Win+Tab) for a contained feel.
EOF
fi

log "done."
