#!/usr/bin/env bash
# tilewm one-line setup (ML4W style): detect distro, install git, clone
# tilewm, hand off to install.sh. Run from anywhere, even a bare machine:
#
#   bash <(curl -s https://raw.githubusercontent.com/Peter5235252/tilewm/main/setup.sh)
#
# Any arguments are forwarded to install.sh (try --yes for automation).
# Honors TILEWM_DEST to override the checkout location (default ~/tilewm).

set -euo pipefail

REPO_URL="https://github.com/Peter5235252/tilewm.git"
DEST="${TILEWM_DEST:-$HOME/tilewm}"

die() { printf 'setup: error: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -ne 0 ] || die "do not run as root."
if ! sudo -n true 2>/dev/null; then
    if [ ! -t 0 ]; then
        die "sudo needs a password but there is no terminal. Authenticate (sudo -v), then re-run."
    fi
    sudo -v || die "sudo authentication failed."
fi

DISTRO=""
if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    case "${ID:-} ${ID_LIKE:-}" in
        *arch*) DISTRO="arch" ;;
        *fedora*) DISTRO="fedora" ;;
    esac
fi
[ -n "$DISTRO" ] || die "unsupported distro: tilewm supports Arch Linux and Fedora only, for now."

if ! command -v git >/dev/null 2>&1; then
    echo "setup: installing git ..."
    if [ "$DISTRO" = "arch" ]; then
        sudo pacman -S --needed --noconfirm git
    else
        sudo dnf install -y git
    fi
fi

if [ -d "$DEST/.git" ]; then
    echo "setup: using existing checkout at $DEST"
else
    echo "setup: cloning into $DEST ..."
    git clone "$REPO_URL" "$DEST"
fi

exec "$DEST/install.sh" "$@"
