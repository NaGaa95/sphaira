#!/bin/sh
# builds sphaira.nro in the builder image, without devkitPro installed on the
# host. --send hands the result to the console over nxlink and stays attached
# to its log.
#
#   tools/nro-builder/build.sh [Release|Dev|Lite] [--send]
#
# (not tools/builder: .gitignore's build*/ would swallow it.)
#
# Release (the default) is what the release workflow publishes; Dev skips LTO,
# so a one-file change relinks in a fraction of the time.
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
preset=Release
send=

for arg in "$@"; do
    case "$arg" in
        --send) send=1 ;;
        *) preset=$arg ;;
    esac
done

image=sphaira-builder
cache=${XDG_CACHE_HOME:-$HOME/.cache}/sphaira-ccache
mkdir -p "$cache"

# a no-op from docker's layer cache unless the Dockerfile changed.
docker build -q -t "$image" "$here" > /dev/null

# a build directory is configured once: after that `cmake --build` reruns cmake
# itself whenever a CMakeLists.txt changed. the dependencies are pinned, so
# FETCHCONTENT_UPDATES_DISCONNECTED keeps that rerun off the network.
in_image() {
    docker run --rm \
        --user "$(id -u):$(id -g)" \
        -v "$root":/src -v "$cache":/ccache -w /src \
        "$image" sh -c "$1"
}

configure="cmake --preset $preset -DFETCHCONTENT_UPDATES_DISCONNECTED"
compile="cmake --build --preset $preset --parallel $(nproc)"

# flipping that cache entry restamps the subbuilds, so every dependency reruns
# its update and then its PATCH_COMMAND — a plain `git apply` for ftpsrv, which
# fails against a checkout already holding the patch. so each reconfigure below
# gets pristine trees to patch. this also drops anything edited by hand under
# _deps/, which is the other way a build directory gets stranded: a local fixup
# that the next update can no longer stash.
pristine_deps() {
    for src in "$root/build/$preset"/_deps/*-src; do
        [ -d "$src/.git" ] || continue
        git -C "$src" reset --hard -q
        git -C "$src" clean -qfd
    done
}

if [ ! -f "$root/build/$preset/build.ninja" ]; then
    in_image "$configure=ON > /dev/null"
fi

# but a pin that moved (a rebase onto upstream) is a ref the local clone does
# not have, and the disconnected update refuses to fetch it — which strands the
# build directory for good. so: reconfigure connected, once, then put the cache
# entry back so later builds stay offline.
if ! in_image "$compile"; then
    echo "retrying with the dependency updates connected" >&2
    pristine_deps
    in_image "$configure=OFF > /dev/null && $compile"
    pristine_deps
    in_image "$configure=ON > /dev/null"
fi

nro="$root/build/$preset/sphaira.nro"
echo "built $nro"

if [ -n "$send" ]; then
    # line buffered: redirected to a file, nxlink would otherwise hold the
    # last few KB of the log, which after a failure are the lines that matter.
    stdbuf -oL nxlink -s "$nro"
fi
