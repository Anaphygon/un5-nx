# uam on the Switch

[uam](https://github.com/devkitPro/uam) (fincs, zlib licence; Mesa parts MIT) is
devkitPro's deko3d shader compiler. un5-nx builds it into the NRO, so the deko3d
renderer can compile a shader its pack lacks on the console (60-80 ms each),
instead of skipping the draw.

`0001` turns uam into a static library on Switch builds, with one entry point:
`un5_uam_compile(stage, glsl, out_path)`. It also makes the GLSL grammars build
with macOS's bison 2.3.

## Build

```sh
git clone https://github.com/devkitPro/uam ~/SwitchDev/third_party_src/uam
cd ~/SwitchDev/third_party_src/uam && git am ~/SwitchDev/un5-nx/patches/uam/0001-*.patch
# meson and mako from the Mesa build's venv; the Mesa cross file
PATH=../switch-mesa/venv/bin:$PATH meson setup --cross-file ../switch-mesa/crossfile.txt \
  build-switch --buildtype=release -Db_ndebug=true
ninja -C build-switch
```

## Isolating it from Mesa

The NRO also links Mesa, which contains the same GLSL compiler under the same
names. Every global symbol uam defines, and every COMDAT group signature, gets
a `un5uam_` prefix. Only `un5_uam_compile` keeps its name.

Renaming the symbols alone is not enough. Unrenamed group signatures (for
example `…D5Ev`, a class's destructor group) made the linker discard uam's
copies as duplicates of Mesa's. Its vtables then pointed at address 0.

```sh
cd build-switch
NM=aarch64-none-elf-nm RE=aarch64-none-elf-readelf
$NM -g --defined-only libuam.a | awk 'NF==3 && $3!="un5_uam_compile" {print $3}' > defined.txt
$RE -g -W libuam.a | grep -o "COMDAT group section \[[ 0-9]*\] \`[^']*' \[[^]]*\]" \
  | sed 's/.*\[\(.*\)\]$/\1/' > groups.txt
sort -u defined.txt groups.txt | grep -vx un5_uam_compile | awk '{print $1" un5uam_"$1}' > redefine.txt
cp libuam.a libuam-isolated.a && aarch64-none-elf-objcopy --redefine-syms=redefine.txt libuam-isolated.a
```

Link `libuam-isolated.a` into the NRO (ps2-web-recomp's `PS2_EXTRA_LIBRARIES`).

On the console it runs on its own thread with an 8 MB stack
(`gs-pcsx2/src/deko3d/uam_thread.cpp`). Astris (Ryujinx) cannot start a thread
with that stack, so on-console compiling is tested on hardware only.
