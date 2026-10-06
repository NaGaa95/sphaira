# the devkitA64 toolchain the release workflow builds with, plus what makes a
# local rebuild fast: ninja for incremental builds, and ccache so compiled
# objects outlive a wiped build directory or a branch switch. used through
# build.sh, which mounts the repo at /src and the cache at /ccache.
FROM devkitpro/devkita64:latest

RUN apt-get update \
 && apt-get install -y --no-install-recommends ccache texinfo \
 && rm -rf /var/lib/apt/lists/*

# upstream builds against a newer libnx than the image ships (NacpStruct's
# lang_data) and a libgloss with R-YaTian's iosupport changes, both built the
# way its build workflow does. baked in when the image is built, so they only
# move when the image is rebuilt.
RUN git clone --depth 1 -b iosupport https://github.com/R-YaTian/newlib.git /tmp/newlib \
 && cd /tmp/newlib && bash build-libgloss-local.sh \
 && git clone --depth 1 --recurse-submodules https://github.com/switchbrew/libnx.git /tmp/libnx \
 && make -C /tmp/libnx install -j$(nproc) \
 && rm -rf /tmp/newlib /tmp/libnx

# read by cmake when it first configures a build directory.
ENV CMAKE_GENERATOR=Ninja \
    CMAKE_C_COMPILER_LAUNCHER=ccache \
    CMAKE_CXX_COMPILER_LAUNCHER=ccache \
    CCACHE_DIR=/ccache \
    CCACHE_MAXSIZE=5G \
    CCACHE_BASEDIR=/src \
    CCACHE_COMPILERCHECK=content \
    HOME=/tmp
