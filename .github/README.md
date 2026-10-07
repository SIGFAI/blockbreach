# BlockBreach

Minecraft inside Ready or Not: build cover mid-raid, set zombies on the suspects, blow walls with TNT.

**BlockBreach is made by [ImmortalSouull](https://github.com/ImmortalSouull).** All credit for the mod goes to them. It is built on [rehan-remade/universal-modder](https://github.com/rehan-remade/universal-modder) by Rehan and universal-modder contributors.

- Original project: https://github.com/ImmortalSouull/BlockBreach
- Report bugs and ask questions there: https://github.com/ImmortalSouull/BlockBreach/issues
- Upstream release packaged here: [v1.0.0](https://github.com/ImmortalSouull/BlockBreach/releases/tag/v1.0.0) (commit [`ffe5ec8`](https://github.com/ImmortalSouull/BlockBreach/tree/ffe5ec8384ce51b9030fa2edeca0281f60f45bac))

> **Beta.** Nobody at SIGF has played this build yet. Back up your saves.
> Bugs in the mod itself go to the author's issue tracker above; problems with the one-click install go to this repository's issues.

## What you need

- **Ready or Not** ([Steam](https://store.steampowered.com/app/1144200/)): tested by the author on Ready or Not build 133804 (game build number; no Steam build id given), DirectX 11 only.
- **Minecraft**: Java Edition 26.3.
- ue4ss 3.0.1-1161-g6eb3d9bc (author build): in the mod files, installed into Ready or Not by the app (https://github.com/UE4SS-RE/RE-UE4SS).
- reshade 6.8.0: in the mod files, installed into Ready or Not by the app (https://reshade.me/).
- Windows and the [SIGF app](https://sigf.ai). The app installs fabric-loader 0.19.5, fabric-api 0.161.0+26.3 for you.

## Install

In the SIGF app, open **BlockBreach** in the catalog, press **Install**, then **Play**. **Restore** puts your game folders back exactly as they were.
The app follows `mashup.json` in this repository: every download is pinned by sha256. The files come from the release [`v1.0.0`](../../releases/tag/v1.0.0).

### How to play

- Ready or Not as usual, with real Minecraft drawn into the mission: Minecraft's blocks, mobs and explosions appear in the level and fight its people.
- Press Play: Minecraft opens its own world first, then Ready or Not starts in DirectX 11. Start any single-player mission and Minecraft shows up in it.
- NumPad 0 toggles build mode: mouse, wheel and 1-9 go to Minecraft. Left click breaks or attacks, right click places or uses, middle click picks, E inventory.
- Every block you place is a solid wall for you, the squad and the suspects. Zombies hunt suspects and officers; your gun kills Minecraft's mobs too.
- F5 shows or hides Minecraft in the picture, F6 re-levels the ground, F7 draws Minecraft under or over the HUD.

### Good to know

- You need Ready or Not on Steam and Minecraft: Java Edition. Ready or Not must run in DirectX 11 (the app starts it with -dx11; from Steam, pick Play Ready or Not (DirectX 11)). Tested by the author on game build 133804; an update can break it.
- Single player only: with other players in the session the mod switches itself off. Never take it into public multiplayer.
- Installs UE4SS (as dwmapi.dll) and ReShade 6.8.0 (as dxgi.dll) into ReadyOrNot\Binaries\Win64; Restore puts that folder back as it was. Never together with ReadyCraft, and run one "Minecraft X" mod at a time.
- Beta, first release. Report bugs to the author on the upstream issue tracker with ue4ss\UE4SS.log, ue4ss\Mods\RoNPassthrough\history.log and ReShade.log from ReadyOrNot\Binaries\Win64.
- No free SIGF server for this mashup: the Minecraft side drives the world from the same PC as Ready or Not. Play it in its own Minecraft world.

## Not together with ReadyCraft

BlockBreach (UE4SS + ReShade, DirectX 11) and ReadyCraft (version.dll, DirectX 12) both install into Ready or Not's `ReadyOrNot/Binaries/Win64`. Install only one of them: restore the other first.

## What this repository holds

1. The upstream source tree at tag `v1.0.0`, commit [`ffe5ec8384ce51b9030fa2edeca0281f60f45bac`](https://github.com/ImmortalSouull/BlockBreach/tree/ffe5ec8384ce51b9030fa2edeca0281f60f45bac), every file unchanged (same git blobs). Upstream's own `README.md` is there, unchanged; GitHub shows this file (`.github/README.md`) first.
2. Added by SIGF in the same commit: this file, and `sigf/` (the scripts that built the release assets, for reference: they run inside the SIGF repository).
3. `mashup.json`, the SIGF app recipe (the next commit).
4. The release `v1.0.0` (its tag is the first commit):

| Asset | Size | sha256 | What it is |
|---|---|---|---|
| `blockbreach-ron.zip` | 11473267 B | `39649898fb83790bd8e68e8da50f6045951e94c7ccc19f1d776e4f38c2571c8e` | upstream's release payload (`BlockBreach-1.0.0-manual.zip`, `ReadyOrNot/Binaries/Win64/`), every file unchanged: `ue4ss/Mods/RoNPassthrough/dlls/main.dll` (the mod), `dwmapi.dll` + `ue4ss/UE4SS.dll` (the author's build of UE4SS v3.0.1-1161-g6eb3d9bc, MIT), `dxgi.dll` (the official ReShade64.dll 6.8.0, BSD-3-Clause, byte-identical), ReShade.ini/ReShadePreset.ini, the shaders, plus upstream's LICENSE and THIRD_PARTY_NOTICES.md under `blockbreach-licenses/`; into `ReadyOrNot/Binaries/Win64`. |
| `blockbreach.mrpack` | 216029 B | `6f848f77827fcb754064e7f10586956c35bf080629c17648e736481603722ba2` | the Minecraft side: upstream's `blockbreach-passthrough.jar` (Java-WebSocket 1.6.0 inside, MIT) and `options.txt` from release `v1.0.0`, unchanged, with upstream's LICENSE, for Minecraft 26.3 with Fabric Loader 0.19.5; Fabric API 0.161.0+26.3 is a Modrinth download link, not stored here. |

The sha256 of every file inside the zips is in `mashup.json` (`contents`).

## Licenses

| Part | License | Where |
|---|---|---|
| BlockBreach (all of the upstream tree and the release files) | MIT, Copyright 2026 BlockBreach contributors; derived from universal-modder (MIT) | `LICENSE`, `release/` THIRD_PARTY_NOTICES in the zip |
| UE4SS (author build of RE-UE4SS v3.0.1-1161-g6eb3d9bc, in `blockbreach-ron.zip`) | MIT | https://github.com/UE4SS-RE/RE-UE4SS |
| ReShade 6.8.0 (`dxgi.dll` in `blockbreach-ron.zip`) | BSD-3-Clause | https://github.com/crosire/reshade |
| Fabric API (downloaded from Modrinth by the app, not stored here) | Apache-2.0 | https://github.com/FabricMC/fabric |

## Why this repository exists

The SIGF app (https://sigf.ai) installs mods from recipes (`mashup.json`) whose downloads are pinned release files. This repository makes BlockBreach installable in one click, credited to ImmortalSouull. If you are the author and want anything changed or taken down, open an issue here.
