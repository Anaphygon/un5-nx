# Mesa for the Switch, with program binaries

devkitPro's `switch-mesa` 20.1.0-5 builds Mesa for horizon without a disk
shader cache. Gallium then reports no program binary format, so PCSX2's
GLShaderCache stays off and every shader is compiled again on each run.

`0001-program-binaries-on-horizon.patch`, on top of devkitPro's patches:

- `st_extensions.c`: report one program binary format on Switch. Program
  binaries only need the TGSI serialisation, which the horizon build keeps.
- `st_shader_cache.c`: the driver sha1 that stamps each binary comes from a
  disk-cache stub that leaves it unset; compute it from the Mesa version
  instead (bump the tag in it when the driver's output changes).
- `include/c11/threads.h`: devkitA64's newlib now has `timespec_get`.

## Build

```sh
cd ~/SwitchDev/third_party_src/switch-mesa
# sources: mesa-20.1.0-rc3.tar.xz and devkitPro's switch/mesa patches
# (see their PKGBUILD for URLs and sha256 sums)
tar xf mesa-20.1.0-rc3.tar.xz && cd mesa-20.1.0-rc3
patch -p1 -i ../switch-mesa-20.1.0-5.patch
patch -p1 -i ../gl_XML.py.patch
patch -p1 -i ../glX_XML.py.patch
patch -p1 -i ~/SwitchDev/un5-nx/patches/switch-mesa/0001-program-binaries-on-horizon.patch
python3 -m venv ../venv && ../venv/bin/pip install meson==0.61.5 mako
PATH=$PWD/../venv/bin:$PATH meson setup --buildtype=plain \
  --cross-file=~/SwitchDev/un5-nx/patches/switch-mesa/crossfile.txt \
  --default-library=static --prefix=/opt/devkitpro/portlibs/switch \
  --libdir=lib build -Db_ndebug=true
ninja -C build && DESTDIR=$PWD/../stage ninja -C build install
```

Then point the builds at `stage/opt/devkitpro/portlibs/switch/lib`: gs-pcsx2's
`SWITCH_EGL` and `SWITCH_GLAPI`, and ps2-web-recomp's `PS2_EGL`, `PS2_GLAPI`
and the EGL and glapi entries of `PS2_EXTRA_LIBRARIES` (an old libEGL listed
earlier wins the link). `/opt/devkitpro` stays as devkitPro ships it.

Mesa is MIT-licensed; this patch is too.
