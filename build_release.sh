#!/bin/sh
set -e

# builds a preset
build_preset() {
    echo Configuring $1 ...
    cmake --preset $1
    echo Building $1 ...
    cmake --build --preset $1
}

build_preset Release

# needs tools/switch2/setup.sh, SKIP_SWITCH2=1 leaves it out.
if [ -z "$SKIP_SWITCH2" ]; then
    build_preset Switch2
fi

rm -rf out

# sphaira.zip holds both builds, the updater takes the one for its console, see
# InstallUpdate() in main_menu.cpp.

# --- SWITCH --- #
mkdir -p out/switch/sphaira/
cp build/Release/sphaira.nro out/switch/sphaira/sphaira.nro
zip_dirs=switch

# --- SWITCH 2 --- #
# zipped before switch: updaters before 1.1.0 extract every sphaira.nro of the
# zip to the same file, so they install the last one.
if [ -z "$SKIP_SWITCH2" ]; then
    mkdir -p out/nx2/switch/sphaira/
    cp build/Switch2/sphaira.nro out/nx2/switch/sphaira/sphaira.nro
    zip_dirs="nx2 $zip_dirs"
fi

(cd out && zip -r9 sphaira.zip $zip_dirs)
