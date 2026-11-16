#!/bin/sh
# installs what the Switch 2 build needs into $DEVKITPRO, at the tested versions:
# the ounce-devkit (libounce and its cmake toolchain), drakotools and drako3d.
#
#   sh tools/switch2/setup.sh
#
# needs devkitPro with switch-dev, git, make, python3, and meson, ninja, flex,
# bison and a host c++ compiler for drakotools.
set -eu

: "${DEVKITPRO:?Set DEVKITPRO to your devkitPro installation}"

# its install.sh installs devkit 20261115b.
OUNCE_DEVKIT_REF=75cea5538b3a57d8f2ecf53354274cf00a365d20
DRAKOTOOLS_REF=b32fe6e43533e80414f6aa4ad3b899ed077f806c
DRAKO3D_REF=d1f05077667d0eabdd39d8a65483ffcec51807c2

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# fetch NAME URL COMMIT
fetch() {
    echo "== $1 $3"
    git init -q "$work/$1"
    git -C "$work/$1" fetch -q --depth 1 "$2" "$3"
    git -C "$work/$1" checkout -q FETCH_HEAD
}

# runs an install step with sudo when $DEVKITPRO is not writable, as the
# ounce-devkit's install.sh does.
as_owner() {
    if [ -w "$DEVKITPRO" ]; then
        "$@"
    else
        sudo env DEVKITPRO="$DEVKITPRO" PATH="$PATH" "$@"
    fi
}

jobs=$(nproc 2>/dev/null || echo 4)

fetch ounce-devkit https://github.com/ouncebrew/ounce-devkit.git "$OUNCE_DEVKIT_REF"
sh "$work/ounce-devkit/install.sh"

fetch drakotools https://github.com/ouncebrew/drakotools.git "$DRAKOTOOLS_REF"
meson setup "$work/drakotools/build" "$work/drakotools" --prefix="$DEVKITPRO/tools" --bindir=bin
ninja -C "$work/drakotools/build"
as_owner ninja -C "$work/drakotools/build" install

# drako3d's Makefile runs drakodef and drakomme from PATH.
PATH="$DEVKITPRO/tools/bin:$PATH"
export PATH

fetch drako3d https://github.com/ouncebrew/drako3d.git "$DRAKO3D_REF"
make -C "$work/drako3d" -j"$jobs"
as_owner make -C "$work/drako3d" install
