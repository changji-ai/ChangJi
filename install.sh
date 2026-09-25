#!/usr/bin/env bash
# One-line installer for changji (no Docker).
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/changji-ai/ChangJi/main/install.sh | bash
# To install the rolling prerelease that every branch push refreshes:
#   CHANGJI_VERSION=beta bash install.sh
# (The default goes through /releases/latest, and a prerelease is not in there,
# so leaving it unset always gets you the stable build.)
#
# ⚠️ **Since 2026-09-17 there is only one Release** (the user: "from now on
# there is only one release too"): stable is always published on the fixed
# `release` tag, deleted and rebuilt each time. The old
# `CHANGJI_VERSION=v1.2.0` spelling no longer resolves to anything (the `v*` and `ci-v*`
# tags are still in git history, but they have no Release behind them). For
# stable, leave it unset or say `release`. The single fixed Release is what
# **automatic updates** need: update checking has to have one address that does
# not change with the version to fetch `version.json` from.
#
# If you already have a locally built binary:
#   CHANGJI_BINARY=/path/to/changji bash install.sh
#
# Afterwards:
#   changji --doctor      health check
#   changji --port 8080   start the server, then open http://127.0.0.1:8080
#
# **This script installs one binary.** Before stage 8 it created a virtualenv
# and pip-installed a Python package; once the Python engine was removed, what
# it installs became the executable on GitHub Releases that carries the web UI
# and the inference engine inside it. ffmpeg and a CJK font still have to be
# installed — assembly and subtitle burn-in need them, and neither belongs
# inside a single-file binary.

set -euo pipefail

PREFIX="${CHANGJI_PREFIX:-$HOME/.changji}"
# Releases are published from changji-ai/ChangJi, the public engine repository,
# which is also where the packaging pipeline lives.
#
# It used to point at integemjack/changji. That one is **private**, so
# `github.com/.../releases/latest/download` was not reachable for anybody
# outside the project at all — and what they saw was "download failed", which
# does not look like a repository problem. Set CHANGJI_REPO to install from
# somewhere else.
REPO="${CHANGJI_REPO:-changji-ai/ChangJi}"
VERSION="${CHANGJI_VERSION:-latest}"

info()  { printf '\033[36m==>\033[0m %s\n' "$*"; }
warn()  { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
die()   { printf '\033[31mfailed:\033[0m %s\n' "$*" >&2; exit 1; }

# ---- Working out what this machine is ----

detect_pm() {
  for pm in apt-get dnf yum pacman zypper apk brew; do
    if command -v "$pm" >/dev/null 2>&1; then echo "$pm"; return; fi
  done
  echo ""
}

need_sudo() {
  if [ "$(id -u)" -eq 0 ]; then echo ""; else echo "sudo"; fi
}

install_pkgs() {
  local pm; pm="$(detect_pm)"
  local sudo_cmd; sudo_cmd="$(need_sudo)"
  [ -z "$pm" ] && return 1
  case "$pm" in
    apt-get) $sudo_cmd apt-get update -qq && $sudo_cmd apt-get install -y -qq "$@" ;;
    dnf|yum) $sudo_cmd "$pm" install -y -q "$@" ;;
    pacman)  $sudo_cmd pacman -Sy --noconfirm --quiet "$@" ;;
    zypper)  $sudo_cmd zypper --non-interactive --quiet install "$@" ;;
    apk)     $sudo_cmd apk add --quiet "$@" ;;
    brew)    brew install "$@" ;;
  esac
}

# The release-asset naming rule, matching the matrix in
# .github/workflows/release.yml. Getting it wrong shows up as a 404, and a 404
# does not distinguish "no package for this platform" from "the name is
# misspelt" — so the computed name is printed.
detect_target() {
  local os arch
  case "$(uname -s)" in
    Linux)   os="linux" ;;
    Darwin)  os="macos" ;;
    MINGW*|MSYS*|CYGWIN*) os="windows" ;;
    *) die "unrecognised system $(uname -s). Build it yourself: see README.md" ;;
  esac
  case "$(uname -m)" in
    x86_64|amd64)  arch="x64" ;;
    aarch64|arm64) arch="arm64" ;;
    *) die "unrecognised architecture $(uname -m). Build it yourself: see README.md" ;;
  esac
  echo "${os}-${arch}"
}

# ---- Dependencies ----

check_ffmpeg() {
  if command -v ffmpeg >/dev/null 2>&1 && command -v ffprobe >/dev/null 2>&1; then
    return 0
  fi
  info "ffmpeg not found; trying to install it (assembly depends on it)"
  install_pkgs ffmpeg || {
    warn "could not install ffmpeg automatically. Assembly will not work."
    warn "Install it by hand, then run changji --doctor again."
    return 1
  }
}

check_fonts() {
  # Burning in CJK subtitles needs a CJK font; without one they render as boxes.
  if fc-list 2>/dev/null | grep -qiE "noto sans cjk|source han sans|wqy|pingfang"; then
    return 0
  fi
  # macOS ships PingFang and may well have no fc-list at all. Do not alarm
  # anybody here.
  if [ "$(uname -s)" = "Darwin" ]; then return 0; fi
  info "no CJK font found; trying to install one (needed for subtitle burn-in)"
  install_pkgs fonts-noto-cjk 2>/dev/null \
    || install_pkgs google-noto-sans-cjk-fonts 2>/dev/null \
    || install_pkgs noto-fonts-cjk 2>/dev/null \
    || warn "could not install a CJK font automatically. CJK subtitles may render as boxes."
}

# ---- Fetching the binary ----

download() {
  local url="$1" out="$2"
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL --retry 3 -o "$out" "$url"
  elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$out" "$url"
  else
    die "neither curl nor wget is installed; install one and try again"
  fi
}

# **The checksum has to be verified.** Without it, a truncated download becomes
# a file that installs perfectly and then says "Exec format error" the moment
# it runs — an error that looks nothing like a network problem.
verify_sha256() {
  local file="$1" sums="$2" name="$3"
  local want got
  want="$(awk -v n="$name" '$2 == n || $2 == "*"n {print $1}' "$sums" | head -n1)"
  if [ -z "$want" ]; then
    warn "$name is not listed in SHA256SUMS; skipping verification"
    return 0
  fi
  if command -v sha256sum >/dev/null 2>&1; then
    got="$(sha256sum "$file" | awk '{print $1}')"
  elif command -v shasum >/dev/null 2>&1; then
    got="$(shasum -a 256 "$file" | awk '{print $1}')"
  else
    warn "no sha256sum / shasum available; skipping verification"
    return 0
  fi
  [ "$want" = "$got" ] || die "checksum mismatch (wanted $want, got $got); download it again"
}

fetch_release() {
  local target asset base tmp
  target="$(detect_target)"
  asset="changji-${target}.tar.gz"
  if [ "$VERSION" = "latest" ]; then
    base="https://github.com/${REPO}/releases/latest/download"
  else
    base="https://github.com/${REPO}/releases/download/${VERSION}"
  fi

  tmp="$(mktemp -d)"
  # **The path has to be expanded at the moment the trap is installed, hence
  # the double quotes.** It used to read `trap 'rm -rf "$tmp"' EXIT` — the
  # single quotes leave $tmp to be expanded when EXIT actually fires, by which
  # point fetch_release has long returned and the local tmp is out of scope.
  # `set -u` then raises "tmp: unbound variable" on the spot: the trap body
  # fails, and **the script returns 1 after printing "installation complete"**.
  # The symptom is that the install plainly worked, while
  # `curl … | bash && changji --doctor`, CI and Dockerfiles all break right
  # there — and the error names line 1 of install.sh, because a trap body is
  # evaluated as its own little script, which looks entirely unrelated to
  # installing anything.
  # Second thing it fixed: that ~100 MB temporary directory **was never once
  # deleted**, leaving a copy behind on every install.
  trap "rm -rf '$tmp'" EXIT

  info "downloading $asset ($VERSION)"
  download "${base}/${asset}" "$tmp/$asset" \
    || die "download failed: ${base}/${asset}
There may be no package for this platform, or the version may be wrong. What
has been published is listed at
  https://github.com/${REPO}/releases"

  if download "${base}/SHA256SUMS" "$tmp/SHA256SUMS" 2>/dev/null; then
    verify_sha256 "$tmp/$asset" "$tmp/SHA256SUMS" "$asset"
    info "checksum verified"
  else
    warn "could not fetch SHA256SUMS; skipping verification"
  fi

  tar -xzf "$tmp/$asset" -C "$tmp"
  [ -f "$tmp/changji" ] || die "the archive contains no file called changji; it may be corrupt"
  mkdir -p "$PREFIX/bin"
  install -m 755 "$tmp/changji" "$PREFIX/bin/changji"
}

# ---- Install ----

main() {
  info "installing into $PREFIX"

  check_ffmpeg || true
  check_fonts || true

  if [ -n "${CHANGJI_BINARY:-}" ]; then
    [ -f "$CHANGJI_BINARY" ] || die "CHANGJI_BINARY points at a file that does not exist: $CHANGJI_BINARY"
    info "using the local binary $CHANGJI_BINARY"
    mkdir -p "$PREFIX/bin"
    install -m 755 "$CHANGJI_BINARY" "$PREFIX/bin/changji"
  else
    fetch_release
  fi

  # Drop a symlink somewhere that is usually on PATH.
  local bindir="$HOME/.local/bin"
  mkdir -p "$bindir"
  ln -sf "$PREFIX/bin/changji" "$bindir/changji"

  info "installed $("$PREFIX/bin/changji" --version 2>/dev/null || echo 'a binary')"
  info "installation complete"
  echo
  if ! echo ":$PATH:" | grep -q ":$bindir:"; then
    warn "$bindir is not on your PATH. Add this line to your shell configuration:"
    echo "    export PATH=\"\$HOME/.local/bin:\$PATH\""
    echo
  fi
  echo "Next:"
  echo "    changji --doctor           check the environment; it names anything missing"
  echo "    changji --port 8080        start the server, then open http://127.0.0.1:8080"
  echo
  echo "The first time you open it, it asks you to choose models to download — image"
  echo "generation, video, screenwriting and voice each need their own weights. Pick"
  echo "them in the interface and it fetches them itself. To use a model running on"
  echo "another machine, put its address in the settings page."
}

main "$@"
