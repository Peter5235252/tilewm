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

DISTRO=""
if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    case "${ID:-} ${ID_LIKE:-}" in
        *arch*) DISTRO="arch" ;;
        *fedora*) DISTRO="fedora" ;;
        *nixos*) DISTRO="nixos" ;;
    esac
fi
[ -n "$DISTRO" ] || die "unsupported distro: tilewm supports Arch Linux, Fedora and NixOS only, for now."

# sudo is only needed for system packages (Arch/Fedora). NixOS installs
# everything user-local via profiles, so skip it there entirely.
if [ "$DISTRO" != "nixos" ]; then
    if ! sudo -n true 2>/dev/null; then
        if [ ! -t 0 ]; then
            die "sudo needs a password but there is no terminal. Authenticate (sudo -v), then re-run."
        fi
        sudo -v || die "sudo authentication failed."
    fi
fi

if [ "$DISTRO" = "nixos" ]; then
    # On NixOS there are no system packages to install: the flake provides
    # the toolchain. We only need nix itself (with flakes) and a git.
    command -v nix >/dev/null 2>&1 || die "Nix is not installed. Install it first: bash <(curl -s https://install.determinate.systems/nix) -- then re-run this script."
    if ! command -v git >/dev/null 2>&1; then
        echo "setup: installing git into your nix profile ..."
        nix profile install nixpkgs#git
    fi
else
    if ! command -v git >/dev/null 2>&1; then
        echo "setup: installing git ..."
        if [ "$DISTRO" = "arch" ]; then
            sudo pacman -S --needed --noconfirm git
        else
            sudo dnf install -y git
        fi
    fi
fi

if [ -d "$DEST/.git" ]; then
    echo "setup: using existing checkout at $DEST"
else
    echo "setup: cloning into $DEST ..."
    git clone "$REPO_URL" "$DEST"
fi

exec "$DEST/install.sh" "$@"
