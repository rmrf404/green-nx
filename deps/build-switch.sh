#!/usr/bin/env bash
# Reproducible cross-compilation of the libpeer WebRTC stack for Nintendo Switch.
#
# Builds (in order): mbedtls (libpeer's pinned 3.4.0, with DTLS-SRTP enabled),
# usrsctp, libsrtp (mbedtls crypto backend) and libpeer itself (peer connection
# core only -- no coreHTTP/coreMQTT signaling), and installs everything into
# deps/switch/ (lib/*.a + include/).
#
# Usage (non-interactive):  bash deps/build-switch.sh
# Requires: docker (devkitpro/devkita64 image), git.
#
# Internal: "--inside" is passed when the script re-executes itself inside the
# devkitpro/devkita64 container.

set -euo pipefail

LIBPEER_REPO="https://github.com/sepfy/libpeer"
DOCKER_IMAGE="devkitpro/devkita64"

# ---------------------------------------------------------------------------
# Container side: do the actual builds.
# ---------------------------------------------------------------------------
if [[ "${1:-}" == "--inside" ]]; then
  export PATH="/opt/devkitpro/portlibs/switch/bin:$PATH"
  DEPS=/work/deps
  PREFIX="$DEPS/switch"
  SHIM="$DEPS/shim/include"
  SRC="$DEPS/src/libpeer"
  JOBS="$(nproc)"

  build() { # name srcdir extra-cflags... -- cmake-args...
    local name="$1" dir="$2"; shift 2
    local cflags=()
    while [[ $# -gt 0 && "$1" != "--" ]]; do cflags+=("$1"); shift; done
    [[ "${1:-}" == "--" ]] && shift
    echo "=== [$name] configure"
    rm -rf "$dir/build-switch"
    CFLAGS="${cflags[*]:-}" aarch64-none-elf-cmake -S "$dir" -B "$dir/build-switch" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      "$@"
    echo "=== [$name] build"
    cmake --build "$dir/build-switch" -j"$JOBS"
    echo "=== [$name] install"
    cmake --install "$dir/build-switch"
  }

  # 1) mbedtls 3.4.0 (libpeer's pinned submodule), DTLS-SRTP enabled via patch.
  #    NOTE: passing extra CFLAGS through the environment (not -DCMAKE_C_FLAGS)
  #    preserves the toolchain's arch flags (-march=... -mtp=soft -D__SWITCH__).
  build mbedtls "$SRC/third_party/mbedtls" -- \
    -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DGEN_FILES=OFF

  # 2) usrsctp (IPv4 only; Switch/libnx has no usable IPv6 stack or headers).
  build usrsctp "$SRC/third_party/usrsctp" \
    -D_DEFAULT_SOURCE -D_BSD_SOURCE "-I$SHIM" -include switch_missing.h -- \
    -Dsctp_werror=0 -Dsctp_build_programs=0 -Dsctp_build_shared_lib=0 \
    -Dsctp_debug=0 -Dsctp_inet6=0

  # 3) libsrtp with the mbedtls crypto engine built in step 1.
  build libsrtp "$SRC/third_party/libsrtp" \
    "-I$PREFIX/include" "-I$SHIM" -include switch_missing.h -- \
    -DENABLE_MBEDTLS=ON -DTEST_APPS=OFF -DBUILD_SHARED_LIBS=OFF \
    -DMBEDTLS_ROOT_DIR="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX"

  # 4) libpeer core (signaling disabled -> no coreHTTP/coreMQTT/cJSON).
  #    LOG_REDIRECT routes libpeer's internal logs to our peer_log() (in
  #    switch_compat.c). LOG_LEVEL=2 (INFO) keeps one-time connection
  #    diagnostics but compiles out the per-packet DEBUG logs -- at DEBUG the
  #    per-RTP-packet logging floods the SD-card log and stalls the worker
  #    thread, wrecking video throughput.
  build libpeer "$SRC" \
    "-I$PREFIX/include" "-I$SHIM" -DLOG_REDIRECT=1 -DLOG_LEVEL=2 -- \
    -DDISABLE_PEER_SIGNALING=ON -DCMAKE_PREFIX_PATH="$PREFIX"

  # 5) Link smoke test: catches unresolved symbols early.
  echo "=== smoke test link"
  cat > /tmp/smoketest.c <<'EOF'
#include <peer.h>
#include <peer_connection.h>
int main(void) {
  peer_init();
  PeerConfiguration config = {0};
  PeerConnection* pc = peer_connection_create(&config);
  peer_connection_create_offer(pc);
  peer_connection_loop(pc);
  peer_connection_destroy(pc);
  peer_deinit();
  return 0;
}
EOF
  /opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc \
    -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -D__SWITCH__ \
    -I"$PREFIX/include" -I/opt/devkitpro/libnx/include \
    /tmp/smoketest.c \
    -specs=/opt/devkitpro/libnx/switch.specs \
    -L"$PREFIX/lib" -L/opt/devkitpro/libnx/lib -L/opt/devkitpro/portlibs/switch/lib \
    -lpeer -lsrtp2 -lusrsctp -lmbedtls -lmbedx509 -lmbedcrypto -lnx -lm \
    -o /tmp/smoketest.elf
  echo "=== smoke test link OK"
  exit 0
fi

# ---------------------------------------------------------------------------
# Host side: clone, patch, run container, verify, fix ownership.
# ---------------------------------------------------------------------------
DEPS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIBPEER_DIR="$DEPS_DIR/src/libpeer"

command -v docker >/dev/null || { echo "docker is required" >&2; exit 1; }
command -v git >/dev/null || { echo "git is required" >&2; exit 1; }

# 1) Clone (shallow, with submodules) if missing.
if [[ ! -e "$LIBPEER_DIR/.git" ]]; then
  mkdir -p "$DEPS_DIR/src"
  git clone --recursive --depth 1 --shallow-submodules "$LIBPEER_REPO" "$LIBPEER_DIR"
fi

# 2) Apply patches (idempotent: skipped when already applied).
apply_patch() {
  local dir="$1" patch="$2"
  if git -C "$dir" apply --reverse --check "$patch" >/dev/null 2>&1; then
    echo "patch already applied: $(basename "$patch")"
  else
    echo "applying patch: $(basename "$patch")"
    git -C "$dir" apply "$patch"
  fi
}
apply_patch "$LIBPEER_DIR" "$DEPS_DIR/patches/libpeer-switch.patch"
apply_patch "$LIBPEER_DIR/third_party/mbedtls" "$DEPS_DIR/patches/mbedtls-switch.patch"
apply_patch "$LIBPEER_DIR/third_party/usrsctp" "$DEPS_DIR/patches/usrsctp-switch.patch"

# 3) Build everything inside the devkitPro container.
rm -rf "$DEPS_DIR/switch"
docker run --rm -v "$DEPS_DIR":/work/deps "$DOCKER_IMAGE" \
  bash /work/deps/build-switch.sh --inside

# 4) Fix ownership of files the container may have created as root
#    (no-op on Docker Desktop for Mac, needed on Linux hosts).
docker run --rm -v "$DEPS_DIR":/work/deps "$DOCKER_IMAGE" \
  chown -R "$(id -u):$(id -g)" /work/deps/switch /work/deps/src >/dev/null 2>&1 || true

# 5) Verify artifacts.
echo
echo "=== installed artifacts ==="
for lib in libpeer.a libsrtp2.a libusrsctp.a libmbedtls.a libmbedcrypto.a libmbedx509.a; do
  if [[ -f "$DEPS_DIR/switch/lib/$lib" ]]; then
    echo "OK  $DEPS_DIR/switch/lib/$lib ($(ar t "$DEPS_DIR/switch/lib/$lib" | wc -l | tr -d ' ') objects)"
  else
    echo "MISSING  $DEPS_DIR/switch/lib/$lib" >&2
    exit 1
  fi
done
echo
echo "Done. Headers in $DEPS_DIR/switch/include, libraries in $DEPS_DIR/switch/lib."
