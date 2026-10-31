#!/usr/bin/env bash
# Orion Control — one-shot installer for Linux.
#
# Does everything a Windows-to-Linux newcomer would otherwise struggle with:
#   * installs the few runtime libraries the AppImage needs
#   * gives your user serial-port access (dialout/uucp group)
#   * downloads the latest console AppImage from GitHub Releases
#   * drops a desktop launcher + icon so it shows up in your menu
#   * LAST, and only if you want it: installs the SDRplay API (the one
#     proprietary piece — from SDRplay; needed for the panadapter only)
#
# Nothing here is fatal except "no release to download": every optional
# piece that fails prints what to do and the script carries on, so you
# always end up with a runnable console.
#
# Usage:   bash install.sh [--no-sdrplay] [--build-deps]
#   --no-sdrplay   skip the SDRplay API step (radio-only use, or you have it)
#   --build-deps   instead of the AppImage, install everything needed to
#                  BUILD the console from source on this machine, then stop
# Re-run any time to update to the newest release.
set -u

REPO="n8mus/Orion-Control"
APPDIR="$HOME/.local/bin"
ICONDIR="$HOME/.local/share/icons/hicolor/256x256/apps"
DESKDIR="$HOME/.local/share/applications"
APP="$APPDIR/tentec-console.AppImage"
NEED_GLIBC="2.38"            # what the published AppImage links against
USER="${USER:-$(id -un)}"

WANT_SDRPLAY=1; BUILD_DEPS=0
for a in "$@"; do
  case "$a" in
    --no-sdrplay) WANT_SDRPLAY=0 ;;
    --build-deps) BUILD_DEPS=1 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown option: $a"; exit 2 ;;
  esac
done
[ "${TTC_NO_SDRPLAY:-0}" = 1 ] && WANT_SDRPLAY=0

say()  { printf '\n\033[1;36m==>\033[0m %s\n' "$1"; }
warn() { printf '\033[1;33m!  %s\033[0m\n' "$1"; }
die()  { printf '\033[1;31mERROR: %s\033[0m\n' "$1" >&2; exit 1; }

[ "$(id -u)" -ne 0 ] || die "Run as your normal user, not root (it will sudo when needed)."

# ---- 1. distro detection --------------------------------------------------
say "Detecting your Linux distribution..."
if   command -v apt-get >/dev/null; then PKG=apt
elif command -v pacman  >/dev/null; then PKG=pacman
elif command -v dnf     >/dev/null; then PKG=dnf
elif command -v zypper  >/dev/null; then PKG=zypper
else warn "Unknown package manager — skipping package installs; the AppImage may still run."; PKG=none
fi
echo "    package manager: $PKG"

# ---- 1b. --build-deps: source-build toolchain, then stop --------------------
if [ "$BUILD_DEPS" = 1 ]; then
  say "Installing everything needed to build Orion Control from source..."
  case "$PKG" in
    apt)    sudo apt-get update -qq
            sudo apt-get install -y --no-install-recommends \
              git cmake g++ make pkg-config qt6-base-dev qt6-serialport-dev \
              qt6-websockets-dev libqt6sql6-sqlite libgl1-mesa-dev \
              libfftw3-dev autoconf automake libtool curl patch \
              qt6-shadertools-dev qt6-base-private-dev 2>/dev/null || \
            sudo apt-get install -y --no-install-recommends \
              git cmake g++ make pkg-config qt6-base-dev qt6-serialport-dev \
              qt6-websockets-dev libqt6sql6-sqlite libgl1-mesa-dev \
              libfftw3-dev autoconf automake libtool curl patch
            # Debian/Ubuntu ship no rnnoise package: build Xiph's release.
            if ! ldconfig -p | grep -q librnnoise; then
              say "Building rnnoise from source (Debian/Ubuntu don't package it)..."
              T="$(mktemp -d)"
              ( git clone --depth 1 https://gitlab.xiph.org/xiph/rnnoise.git "$T/rnnoise" || \
                git clone --depth 1 https://github.com/xiph/rnnoise.git "$T/rnnoise" ) && \
              ( cd "$T/rnnoise" && ./autogen.sh && ./configure --prefix=/usr/local && \
                make -j"$(nproc)" && sudo make install && sudo ldconfig ) || \
                warn "rnnoise build failed — the console needs it; see README 'Build from source'."
              rm -rf "$T"
            fi ;;
    pacman) sudo pacman -Sy --needed --noconfirm \
              git cmake gcc make pkgconf qt6-base qt6-serialport qt6-websockets \
              qt6-shadertools rnnoise fftw autoconf automake libtool curl patch ;;
    dnf)    sudo dnf install -y \
              git cmake gcc-c++ make pkgconf qt6-qtbase-devel qt6-qtserialport-devel \
              qt6-qtwebsockets-devel qt6-qtshadertools-devel qt6-qtbase-private-devel \
              rnnoise-devel fftw-devel autoconf automake libtool curl patch ;;
    zypper) sudo zypper -n install \
              git cmake gcc-c++ make pkgconf qt6-base-devel qt6-serialport-devel \
              qt6-websockets-devel qt6-shadertools-devel rnnoise-devel fftw3-devel \
              autoconf automake libtool curl patch ;;
    *)      die "Unknown package manager — see README 'Build from source' for the list." ;;
  esac
  cat <<EOF

  Build tools installed. Now:

    git clone https://github.com/$REPO.git
    cd Orion-Control
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release        # add -DBUILD_SDRPLAY=ON once the SDRplay API is installed
    cmake --build build -j\$(nproc)
    ./build/tentec-console

  Without the SDRplay API the console builds and runs radio-only (dark
  panadapter). Run 'bash install.sh' (no flags) later to add the API.
EOF
  exit 0
fi

# ---- 2. runtime libraries -------------------------------------------------
say "Installing runtime libraries (needs your sudo password)..."
case "$PKG" in
  apt)    sudo apt-get update -qq
          sudo apt-get install -y --no-install-recommends \
              libfuse2 libgl1 libxcb-cursor0 curl ca-certificates hamlib-utils || \
          sudo apt-get install -y --no-install-recommends \
              libfuse2t64 libgl1 libxcb-cursor0 curl ca-certificates hamlib-utils || \
              warn "Some libraries failed; the AppImage bundles Qt itself, so it may still run." ;;
  pacman) sudo pacman -Sy --needed --noconfirm fuse2 curl xcb-util-cursor hamlib || \
              warn "Some libraries failed; the AppImage bundles Qt itself, so it may still run." ;;
  dnf)    sudo dnf install -y fuse fuse-libs curl xcb-util-cursor hamlib || \
              warn "Some libraries failed; the AppImage bundles Qt itself, so it may still run." ;;
  zypper) sudo zypper -n install libfuse2 curl libxcb-cursor0 hamlib || \
              warn "Some libraries failed; the AppImage bundles Qt itself, so it may still run." ;;
esac
command -v curl >/dev/null || die "curl is required (install it and re-run)."

# ---- 3. glibc check ---------------------------------------------------------
HAVE_GLIBC="$(ldd --version 2>/dev/null | sed -n '1s/.* \([0-9][0-9]*\.[0-9][0-9]*\)$/\1/p')"
HAVE_GLIBC="${HAVE_GLIBC:-0}"
if [ "$(printf '%s\n%s\n' "$NEED_GLIBC" "$HAVE_GLIBC" | sort -V | head -1)" != "$NEED_GLIBC" ]; then
  warn "This system has glibc $HAVE_GLIBC; the AppImage needs >= $NEED_GLIBC"
  warn "(Ubuntu 24.04+, Debian 13+, Fedora 39+, current Arch/openSUSE Tumbleweed)."
  warn "It will probably not start here. Build from source instead:"
  warn "    bash install.sh --build-deps"
  warn "Continuing anyway in case I'm wrong."
fi

# ---- 4. serial-port access ------------------------------------------------
say "Granting your user access to serial ports (radio CAT)..."
GRP=dialout; getent group dialout >/dev/null || GRP=uucp   # Arch/Fedora use uucp
if id -nG "$USER" | tr ' ' '\n' | grep -qx "$GRP"; then
  echo "    already in group '$GRP'."
else
  sudo usermod -aG "$GRP" "$USER" && \
    warn "Added you to '$GRP'. LOG OUT AND BACK IN (or reboot) for serial to work." || \
    warn "Could not add you to '$GRP' — run: sudo usermod -aG $GRP $USER"
fi

# ---- 5. download the latest console AppImage ------------------------------
say "Fetching the latest console release from GitHub..."
mkdir -p "$APPDIR" "$ICONDIR" "$DESKDIR"
ASSET_URL=""
# Primary: the releases API. Newest release with an AppImage wins, pre-release
# or not. Parse with jq, else python3, else grep — none of them is required.
API_JSON="$(curl -fsSL -H 'Accept: application/vnd.github+json' \
            "https://api.github.com/repos/$REPO/releases?per_page=10" 2>/dev/null || true)"
if [ -n "$API_JSON" ]; then
  if command -v jq >/dev/null; then
    ASSET_URL="$(printf '%s' "$API_JSON" | jq -r '[.[] | .assets[]? | select(.name|endswith(".AppImage")) | .browser_download_url][0] // empty')"
  elif command -v python3 >/dev/null; then
    ASSET_URL="$(printf '%s' "$API_JSON" | python3 -I -c '
import json,sys
for r in json.load(sys.stdin):
    for a in r.get("assets",[]):
        if a["name"].endswith(".AppImage"): print(a["browser_download_url"]); sys.exit()' 2>/dev/null)"
  else
    ASSET_URL="$(printf '%s' "$API_JSON" | grep -oE 'https://[^"]+\.AppImage' | head -1)"
  fi
fi
# Fallback (API rate-limited or down): the release web pages.
if [ -z "$ASSET_URL" ]; then
  warn "GitHub API unavailable (rate limit?) — reading the releases page instead."
  for TAG in $(curl -fsSL "https://github.com/$REPO/releases" 2>/dev/null \
               | grep -oE "/$REPO/releases/tag/[^\"/]+" | sed 's#.*/##' | awk '!s[$0]++' | head -5); do
    ASSET_URL="$(curl -fsSL "https://github.com/$REPO/releases/expanded_assets/$TAG" 2>/dev/null \
                 | grep -oE "/$REPO/releases/download/[^\"]+\.AppImage" | head -1)"
    [ -n "$ASSET_URL" ] && { ASSET_URL="https://github.com$ASSET_URL"; break; }
  done
fi
[ -n "$ASSET_URL" ] || die "Could not find an AppImage to download. Check https://github.com/$REPO/releases"
echo "    $ASSET_URL"
curl -fSL --progress-bar "$ASSET_URL" -o "$APP.part" || die "Download failed. Check your network and retry."
mv -f "$APP.part" "$APP"
chmod +x "$APP"
echo "    installed: $APP"

# ---- 6. icon + desktop launcher ------------------------------------------
say "Creating the menu launcher..."
EXEC="$APP"
# No FUSE 2 on this box? AppImages still run, by unpacking themselves first.
LDCACHE="$(ldconfig -p 2>/dev/null || true)"
if ! printf '%s' "$LDCACHE" | grep -q 'libfuse\.so\.2'; then
  warn "libfuse2 not found — the launcher will use --appimage-extract-and-run (slower start, works everywhere)."
  EXEC="$APP --appimage-extract-and-run"
fi
T="$(mktemp -d)"
( cd "$T" && "$APP" --appimage-extract 'usr/share/icons/hicolor/256x256/apps/tentec-console.png' >/dev/null 2>&1 ) || true
PNG="$(find "$T" -name tentec-console.png | head -1)"
[ -n "$PNG" ] && cp "$PNG" "$ICONDIR/tentec-console.png"
rm -rf "$T"
cat > "$DESKDIR/tentec-console.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Orion Control
Comment=SDR panadapter and control for Ten-Tec Orion / Omni VII
Exec=$EXEC
Icon=tentec-console
Categories=HamRadio;Audio;Network;
Terminal=false
EOF
update-desktop-database "$DESKDIR" >/dev/null 2>&1 || true

# ---- 7. SDRplay API (proprietary — from SDRplay, not bundled) — LAST --------
# Last on purpose: its installer is interactive (license prompt, sudo) and
# nothing above depends on it. Without it the console runs radio-only.
if [ "$WANT_SDRPLAY" = 0 ]; then
  say "Skipping the SDRplay API (--no-sdrplay)."
elif ls /usr/local/lib/libsdrplay_api.so.3* /usr/lib/libsdrplay_api.so.3* /usr/lib64/libsdrplay_api.so.3* >/dev/null 2>&1; then
  say "SDRplay API already installed — good."
elif [ ! -t 0 ]; then
  say "SDRplay API: not installing — no terminal to answer its license prompt."
  warn "The panadapter needs it. When you're at a keyboard, run:  bash install.sh"
  warn "or install it yourself from https://www.sdrplay.com/api/"
else
  say "Installing the SDRplay API (needed for the panadapter; radio control works without it)..."
  echo "    This is SDRplay's own installer; it asks you to accept their license."
  VER="3.15.2"
  RUN="SDRplay_RSP_API-Linux-${VER}.run"
  TMP="$(mktemp -d)"
  # SDRplay has pulled and restored the standalone API download before; the
  # Internet Archive holds the exact bytes of the vendor file as fallback.
  if curl -fSL "https://www.sdrplay.com/software/${RUN}" -o "$TMP/$RUN" 2>/dev/null || \
     curl -fSL "https://web.archive.org/web/20260805142854id_/https://www.sdrplay.com/software/${RUN}" -o "$TMP/$RUN"; then
      chmod +x "$TMP/$RUN"
      echo "    Launching SDRplay's installer — follow its prompts (accept the license)."
      sudo "$TMP/$RUN" </dev/tty || warn "SDRplay installer did not finish; the console still runs radio-only. Re-run install.sh to retry."
      sudo systemctl enable --now sdrplay 2>/dev/null || true
  else
      warn "Could not download the SDRplay API."
      warn "Install it yourself from https://www.sdrplay.com/api/ then re-run this script."
  fi
  rm -rf "$TMP"
fi

say "Done!"
cat <<EOF

  Orion Control is installed and in your applications menu.
  You can also run it directly:  $APP

  If this was the first install, LOG OUT and back in once so serial-port
  access takes effect. First launch opens a Setup screen for your radio port,
  callsign and audio. Re-run this script any time to update to a new release.
EOF
