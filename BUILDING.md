# Building BlockBreach

Most people only need the release: **[Releases](https://github.com/ImmortalSouull/BlockBreach/releases) → BlockBreach-1.0.0.zip → BlockBreach.exe**.

The pieces and what builds them:

| Folder | What | Build |
|---|---|---|
| `ron-host/` | The Ready or Not side: a UE4SS C++ mod + ReShade add-on in one DLL (camera link, compositor, RoN's collision as Minecraft barriers, cross-game combat) and the `MCPassthrough.fx` shader | CMake + Ninja + MSVC, inside a checkout of [RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) (`CMakeLists.root.txt` is the top-level file that adds UE4SS and this mod; `build-host.bat` runs it). Needs the ReShade add-on headers ([ReShade](https://github.com/crosire/reshade) `include/`). |
| `minecraft-mod/` | The Minecraft side: a Fabric mod for Minecraft 26.3 (camera, frame export, barriers, mobs, inventory, build mode) | `gradlew build` with JDK 25 |
| `installer/` | BlockBreach.exe: installer + launcher (Rust, egui) with the mod embedded | `python make_payload.py` (collects the builds above, UE4SS, the ReShade add-on build `dxgi.dll`, Fabric API) → `cargo build --release`; `BlockBreach.exe --selftest <dir>` checks install/repair/uninstall on fake game folders |
| `release/` | `build_all.py` runs everything in order, `make_release.py` makes the zips, `scan_paths.py` checks no build-machine paths ship | Python 3 |
| `video/` | The showcase edit (`make_tiktok3.py`) and its synthesised music (`gen_music.py`) | Python 3 + Pillow + ffmpeg |

The build scripts were written for the author's workspace layout (paths in `make_payload.py` / `build_all.py`); adjust them to yours.
The Minecraft passthrough mod and the compositor are derived from rehan's *mc-gta5-passthrough* example (MIT).
