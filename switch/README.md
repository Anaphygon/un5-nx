# Files for the SD card that are not the game

`shaders-dk/` is the deko3d shader pack: PCSX2's OpenGL GS shaders (GPL-3.0)
compiled for the Switch GPU with devkitPro's uam, one `<fnv64 of the GLSL>.dksh`
per program variant UN5 draws with. It holds no game data. The NRO compiles a
variant the pack lacks on the console (60-80 ms each) and adds it on the SD
card; `tools/dk_sync.sh` keeps the pack in this repository current.

The builder (`docker/`) copies it to `sdmc:/switch/un5-nx/shaders-dk/`, next to
PCSX2's shader sources in `resources/shaders/`.
