#!/usr/bin/env bash
# AetherConsole / Ten-Tec SDR Console — one-shot installer for Linux.
#
# Does everything a Windows-to-Linux newcomer would otherwise struggle with:
#   * installs the few runtime libraries the AppImage needs
#   * installs the SDRplay API (the one proprietary piece — from SDRplay)
#   * gives your user serial-port access (dialout/uucp group)
#   * downloads the latest console AppImage from GitHub Releases
#   * drops a desktop launcher + icon so it shows up in your menu
#
# Usage:   bash install.sh
# Re-run any time to update to the newest release.
set -euo pipefail

REPO="n8mus/orion3-sdr-console"
APPDIR="$HOME/.local/bin"
ICONDIR="$HOME/.local/share/icons/hicolor/256x256/apps"
DESKDIR="$HOME/.local/share/applications"
APP="$APPDIR/tentec-console.AppImage"

say()  { printf '\n\033[1;36m==>\033[0m %s\n' "$1"; }
warn() { printf '\033[1;33m!  %s\033[0m\n' "$1"; }
die()  { printf '\033[1;31mERROR: %s\033[0m\n' "$1" >&2; exit 1; }

[ "$(id -u)" -ne 0 ] || die "Run as your normal user, not root (it will sudo when needed)."

# ---- 1. distro detection + runtime libraries -----------------------------
say "Detecting your Linux distribution..."
if   command -v apt-get >/dev/null; then PKG=apt
elif command -v pacman  >/dev/null; then PKG=pacman
elif command -v dnf     >/dev/null; then PKG=dnf
else warn "Unknown package manager — skipping library install; the AppImage may still run."; PKG=none
fi
echo "    package manager: $PKG"

say "Installing runtime libraries (needs your sudo password)..."
case "$PKG" in
  apt)    sudo apt-get update -qq
          sudo apt-get install -y --no-install-recommends \
              libqt6core6 libqt6gui6 libqt6widgets6 libqt6network6 \
              libqt6serialport6 libfuse2 libgl1 curl jq hamlib-utils || \
              warn "Some libraries failed; the AppImage bundles most of Qt anyway." ;;
  pacman) sudo pacman -Sy --needed --noconfirm \
              qt6-base qt6-serialport fuse2 curl jq hamlib || \
              warn "Some libraries failed; the AppImage bundles most of Qt anyway." ;;
  dnf)    sudo dnf install -y \
              qt6-qtbase qt6-qtserialport fuse curl jq hamlib || \
              warn "Some libraries failed; the AppImage bundles most of Qt anyway." ;;
esac

# ---- 2. serial-port access ------------------------------------------------
say "Granting your user access to serial ports (radio CAT)..."
GRP=dialout; getent group dialout >/dev/null || GRP=uucp   # Arch/Fedora use uucp
if id -nG "$USER" | tr ' ' '\n' | grep -qx "$GRP"; then
  echo "    already in group '$GRP'."
else
  sudo usermod -aG "$GRP" "$USER"
  warn "Added you to '$GRP'. LOG OUT AND BACK IN (or reboot) for serial to work."
fi

# ---- 3. SDRplay API (proprietary — from SDRplay, not bundled) -------------
if [ -e /usr/local/lib/libsdrplay_api.so.3 ] || [ -e /usr/lib/libsdrplay_api.so.3 ]; then
  say "SDRplay API already installed — good."
else
  say "Installing the SDRplay API (required for the panadapter)..."
  echo "    This is SDRplay's own driver; you'll accept their license."
  VER="3.15.2"
  RUN="SDRplay_RSP_API-Linux-${VER}.run"
  URL="https://www.sdrplay.com/software/${RUN}"
  TMP="$(mktemp -d)"
  if curl -fSL "$URL" -o "$TMP/$RUN"; then
      chmod +x "$TMP/$RUN"
      echo "    Launching SDRplay's installer — follow its prompts (accept the license)."
      sudo "$TMP/$RUN" || warn "SDRplay installer returned non-zero; check its output above."
  else
      warn "Could not auto-download the SDRplay API."
      warn "Install it yourself from https://www.sdrplay.com/api/ then re-run this script."
  fi
  rm -rf "$TMP"
fi

# ---- 4. download the latest console AppImage ------------------------------
say "Fetching the latest console release from GitHub..."
mkdir -p "$APPDIR" "$ICONDIR" "$DESKDIR"
# /releases (all) not /releases/latest — the latter skips pre-releases, and
# the alpha builds are pre-releases. Newest release with an AppImage wins.
ASSET_URL="$(curl -fsSL "https://api.github.com/repos/$REPO/releases" \
             | jq -r '[.[] | .assets[]? | select(.name|endswith(".AppImage"))
                      | .browser_download_url][0]')"
[ -n "${ASSET_URL:-}" ] && [ "$ASSET_URL" != "null" ] || \
  die "No AppImage found in the latest release yet. Check https://github.com/$REPO/releases"
curl -fSL "$ASSET_URL" -o "$APP"
chmod +x "$APP"
echo "    installed: $APP"

# ---- 5. icon + desktop launcher ------------------------------------------
say "Creating the menu launcher..."
"$APP" --appimage-extract '*/tentec-console.png' >/dev/null 2>&1 || true
if [ -f squashfs-root/tentec-console.png ]; then
  cp squashfs-root/tentec-console.png "$ICONDIR/tentec-console.png"; rm -rf squashfs-root
fi
cat > "$DESKDIR/tentec-console.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Ten-Tec SDR Console
Comment=SDR panadapter and control for Ten-Tec Orion / Omni VII
Exec=$APP
Icon=tentec-console
Categories=HamRadio;Audio;Network;
Terminal=false
EOF
update-desktop-database "$DESKDIR" >/dev/null 2>&1 || true

say "Done!"
cat <<EOF

  The console is installed and in your applications menu ("Ten-Tec SDR Console").
  You can also run it directly:  $APP

  If this was the first install, LOG OUT and back in once so serial-port
  access takes effect. First launch opens a Setup screen for your radio port,
  callsign and audio. Re-run this script any time to update to a new release.
EOF
