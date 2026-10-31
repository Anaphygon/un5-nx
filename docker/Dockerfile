# SPDX-License-Identifier: GPL-3.0+
# un5-nx builder: everything needed to turn your own copy of Naruto Shippuden:
# Ultimate Ninja 5 (Europe, SLES-55605) into a Nintendo Switch homebrew NRO.
# The image holds tools and libraries only; the game comes in at run time and
# nothing from it stays in the image. See docker/README.md.
#
#   docker build -t un5-nx-builder -f docker/Dockerfile .
#   docker run --rm -v "/path/to/your.iso:/in/game.iso:ro" -v "$PWD/out:/out" un5-nx-builder

# ---- Host tools: ps2-web-recomp's recompilers, linked statically so they run
# ---- on the devkitPro image whatever its C++ library is.
FROM debian:trixie-slim AS host-tools
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates cmake g++ git ninja-build python3 \
    && rm -rf /var/lib/apt/lists/*
ARG PS2WEB_REPO=https://github.com/snuri00/ps2-web-recomp
ARG PS2WEB_PIN=79d8eab
COPY patches/ps2-web-recomp /src/patches/ps2-web-recomp
RUN git clone "$PS2WEB_REPO" /ps2web \
    && git -C /ps2web checkout -q -b un5-nx "$PS2WEB_PIN" \
    && git -C /ps2web -c user.name=un5-nx -c user.email=un5-nx@localhost am -q /src/patches/ps2-web-recomp/*.patch \
    && cmake -S /ps2web -B /ps2web/build/tools -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
    && ninja -C /ps2web/build/tools eerecomp vurecomp

# ---- The Switch toolchain and the libraries the NRO links.
FROM devkitpro/devkita64:latest
RUN apt-get update && apt-get install -y --no-install-recommends \
        bison ca-certificates cmake flex git meson ninja-build patch python3 python3-mako xz-utils \
    && rm -rf /var/lib/apt/lists/* \
    && dkp-pacman -Syu --noconfirm \
    && dkp-pacman -S --needed --noconfirm switch-dev dkp-meson-scripts switch-pkg-config \
        switch-libdrm_nouveau switch-mesa switch-sdl2 switch-freetype switch-harfbuzz switch-libpng switch-zlib switch-bzip2 deko3d \
    && dkp-pacman -Scc --noconfirm
ENV DEVKITPRO=/opt/devkitpro DEVKITA64=/opt/devkitpro/devkitA64 \
    PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH
WORKDIR /un5

# Mesa 20.1 for horizon: devkitPro's switch-mesa 20.1.0-5, with un5-nx's
# program binaries and working glthread (patches/switch-mesa/README.md). The
# packaged switch-mesa above provides the headers; the NRO links these.
COPY patches/switch-mesa /un5/patches/switch-mesa
ADD --checksum=sha256:c90b75ea34302ebde9b81b87c5642fa864c40fe9c4ad34ce0793170c1413168d \
    https://archive.mesa3d.org/older-versions/20.x/mesa-20.1.0-rc3.tar.xz /un5/mesa.tar.xz
RUN tar xf mesa.tar.xz && cd mesa-20.1.0-rc3 \
    && for p in /un5/patches/switch-mesa/devkitpro/switch-mesa-20.1.0-5.patch \
                /un5/patches/switch-mesa/devkitpro/gl_XML.py.patch /un5/patches/switch-mesa/devkitpro/glX_XML.py.patch \
                /un5/patches/switch-mesa/0*.patch; do patch -p1 -i "$p"; done \
    && /opt/devkitpro/meson-cross.sh switch /un5/patches/switch-mesa/crossfile.txt build -Db_ndebug=true \
    && ninja -C build && DESTDIR=/un5/mesa ninja -C build install \
    && cd /un5 && rm -rf mesa-20.1.0-rc3 mesa.tar.xz

# uam, devkitPro's deko3d shader compiler, as a static library whose symbols
# cannot clash with Mesa's (patches/uam/README.md).
COPY patches/uam /un5/patches/uam
RUN git clone https://github.com/devkitPro/uam /un5/uam \
    && git -C /un5/uam checkout -q 104b5b054cfee19206e2fca5fb81f01b0c3abb7b \
    && git -C /un5/uam -c user.name=un5-nx -c user.email=un5-nx@localhost am -q /un5/patches/uam/0001-*.patch \
    && cd /un5/uam && meson setup --cross-file /un5/patches/switch-mesa/crossfile.txt build-switch --buildtype=release -Db_ndebug=true \
    && ninja -C build-switch && cd build-switch \
    && aarch64-none-elf-nm -g --defined-only libuam.a | awk 'NF==3 && $3!="un5_uam_compile" {print $3}' > defined.txt \
    && aarch64-none-elf-readelf -g -W libuam.a | grep -o "COMDAT group section \[[ 0-9]*\] \`[^']*' \[[^]]*\]" \
        | sed 's/.*\[\(.*\)\]$/\1/' > groups.txt \
    && sort -u defined.txt groups.txt | grep -vx un5_uam_compile | awk '{print $1" un5uam_"$1}' > redefine.txt \
    && cp libuam.a /un5/libuam-isolated.a && aarch64-none-elf-objcopy --redefine-syms=redefine.txt /un5/libuam-isolated.a \
    && rm -rf /un5/uam

# PCSX2's GS, the part of PCSX2 un5-nx draws with (gs-pcsx2/README.md).
RUN git clone --filter=blob:none --no-checkout https://github.com/PCSX2/pcsx2 /un5/pcsx2 \
    && git -C /un5/pcsx2 sparse-checkout set 3rdparty/fast_float 3rdparty/fmt 3rdparty/glad 3rdparty/imgui \
        3rdparty/include 3rdparty/xxhash bin/resources/shaders/common bin/resources/shaders/opengl common pcsx2/GS \
    && git -C /un5/pcsx2 checkout -q f0523f8acb1d3a765ee73b74cffec63093569199
COPY gs-pcsx2 /un5/gs-pcsx2
RUN cd /un5/pcsx2 && for p in /un5/gs-pcsx2/patches/*.patch; do patch -p1 -i "$p"; done \
    && cmake -S /un5/gs-pcsx2 -B /un5/gs-pcsx2/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DPCSX2_SRC=/un5/pcsx2 \
    && ninja -C /un5/gs-pcsx2/build pcsx2gs

# ps2-web-recomp with un5-nx's patches, and its recompilers from the first stage.
COPY --from=host-tools /ps2web /un5/ps2web
COPY ports /un5/ports
COPY tools /un5/tools
COPY switch /un5/switch
COPY docker/build.sh /un5/build.sh
ENV PS2WEB=/un5/ps2web
ENTRYPOINT ["/un5/build.sh"]
