# BlockBreach — Ready or Not × Minecraft

**Minecraft Java runs alongside Ready or Not, and its blocks, mobs and explosions appear inside Ready or Not's
picture** — at the right depth, under the HUD, lit to match the scene. Build cover in the middle of a raid, set
zombies on the suspects, blow a wall with TNT.

**Download:** [Releases](https://github.com/ImmortalSouull/BlockBreach/releases) → `BlockBreach-1.0.0.zip` → run `BlockBreach.exe`. Building from source: [BUILDING.md](BUILDING.md).

*(Русская версия ниже.)*

## What it does
- **Minecraft in the picture.** Ready or Not's camera drives Minecraft's; Minecraft's frame is composited into
  Ready or Not's against its depth buffer (ReShade add-on), with lighting and haze matched to the map.
- **Ready or Not's level in Minecraft.** Floors, stairs and walls become invisible barriers, so Minecraft's mobs walk
  every map, take the stairs and go through doorways.
- **Build mode (NumPad 0).** The mouse, wheel and 1–9 go to Minecraft: place blocks on Ready or Not's floor, use any
  item. **E** opens Minecraft's creative inventory with every item. Every placed block is a solid wall in Ready or Not
  (players and AI bump into it, bullets stop).
- **The two games fight.** Arrows, the sword, TNT, creepers and crossbow fireworks hurt Ready or Not's people; your
  own gun kills Minecraft's mobs (headshots count); suspects and your squad shoot zombies; zombies hunt suspects,
  officers and you.
- **Every map** gets its own part of the Minecraft world: what you build stays on that map.

## Requirements
- Windows 10/11, a GPU that runs both games (tested: RTX 4050 laptop, ~65–85 fps at 1920×1200).
- **Ready or Not** (Steam), tested on build 133804. Single-player only.
- **Minecraft: Java Edition** with the official Minecraft Launcher (Microsoft Store / Xbox app or the classic one).
  BlockBreach uses Minecraft 26.3 with Fabric Loader 0.19.5 in its own game folder; the launcher downloads the game
  itself on first start.

## Install
1. Run **BlockBreach.exe**. It finds Ready or Not (through Steam) and the Minecraft Launcher by itself.
2. **DEPLOY** — installs everything (≈30 MB). Files that were already in Ready or Not's folder are backed up.
3. **READY** — opens the Minecraft Launcher with the *BlockBreach* profile selected: press **PLAY** there. Once
   Minecraft has opened its world, BlockBreach starts Ready or Not in DirectX 11 through Steam.
4. Start any mission. Minecraft appears in it.

Windows SmartScreen may warn about an unknown publisher (the exe is not code-signed): *More info → Run anyway*.
Some antivirus programs flag `dwmapi.dll` / `dxgi.dll` (UE4SS and ReShade load through them): that is how both work.

## Controls
| Key | |
|---|---|
| NumPad 0 | Build mode on/off (mouse, wheel and 1–9 go to Minecraft) |
| LMB / RMB | Build mode: break, attack, shoot the bow / place a block, use the item |
| 1–9, wheel | Build mode: hotbar slot |
| MMB | Build mode: pick the block you look at |
| E | Build mode: Minecraft's inventory (E or Esc closes it) |
| Fire | Your gun hits Minecraft's mobs too |
| F5 | Minecraft in the picture on/off |
| F6 | Re-level the ground + test blocks in front of you |
| F7 | Draw Minecraft under / over the HUD |

Hotbar: zombie egg, diamond sword, crossbow (+ fireworks in the off hand), bow, TNT, flint and steel, creeper egg,
grass block, fireworks.

## Settings
BlockBreach → **SETTINGS**: Minecraft's render resolution (75 % by default: more FPS, looks the same for pixel
blocks), whether mobs hunt you, whether your squad fights mobs. Applied the next time Ready or Not starts.

## Uninstall
BlockBreach → **INSTALL → UNINSTALL** (or Windows *Apps & features → BlockBreach*). Everything goes, the backed-up
files go back, the Minecraft profile is removed; your Minecraft world is kept if you ask it to.

## Troubleshooting
- Nothing of Minecraft in Ready or Not: Ready or Not must run in **DirectX 11** (use READY, or Steam → *Play Ready or
  Not (DirectX 11)*), and Minecraft must be in its world before you start a mission. F5 toggles the picture.
- Logs: `…\Ready Or Not\ReadyOrNot\Binaries\Win64\ue4ss\UE4SS.log`, `…\ue4ss\Mods\RoNPassthrough\history.log`,
  `…\Win64\ReShade.log`; Minecraft: `%LOCALAPPDATA%\BlockBreach\minecraft\logs\latest.log`.
- After a Ready or Not update the game may stop loading UE4SS: run BlockBreach → INSTALL → REPAIR, and check for a
  BlockBreach update.
- Another ReShade or UE4SS install in the same folder is backed up and replaced while BlockBreach is installed.

## Credits
- [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) (MIT) — Unreal Engine scripting system.
- [ReShade](https://reshade.me) (BSD 3-Clause) by crosire — the add-on API the compositor runs in.
- [Fabric Loader and Fabric API](https://fabricmc.net) (Apache 2.0).
- The *mc-gta5-passthrough* example by rehan (MIT) — the Minecraft passthrough mod and compositor this builds on.
- Fonts: Oswald, Roboto Condensed, JetBrains Mono, Press Start 2P (SIL Open Font License).
- Built with Claude (Anthropic) in Claude Code: the code, the installer, the art (procedural) and the testing.
- Fan-made. Not affiliated with VOID Interactive, Mojang Studios or Microsoft. *Ready or Not* and *Minecraft* are
  trademarks of their owners.

License: BlockBreach's own code is MIT (see LICENSE). Third-party parts keep their licenses (THIRD_PARTY_NOTICES.md).

---

# BlockBreach — Ready or Not × Minecraft (по-русски)

**Minecraft Java работает рядом с Ready or Not, и его блоки, мобы и взрывы появляются прямо в кадре Ready or Not** —
на правильной глубине, под интерфейсом, с подстроенным светом. Стройте укрытия посреди штурма, натравливайте зомби на
подозреваемых, взрывайте стены динамитом.

## Возможности
- **Minecraft в кадре**: камера RoN управляет камерой Minecraft, кадр Minecraft встраивается по буферу глубины RoN.
- **Уровень RoN в Minecraft**: полы, лестницы и стены — невидимые барьеры, мобы ходят по картам RoN.
- **Режим стройки (NumPad 0)**: мышь, колесо и 1–9 идут в Minecraft; **E** — творческий инвентарь Minecraft со всеми
  предметами. Каждый блок — твёрдая стена в RoN.
- **Игры воюют**: стрелы, меч, TNT, криперы и фейерверки ранят людей RoN; ваше оружие убивает мобов Minecraft;
  подозреваемые и отряд отстреливаются; зомби охотятся на подозреваемых, бойцов и на вас.
- **У каждой карты** своя часть мира Minecraft.

## Требования
Windows 10/11; Ready or Not (Steam, проверено на сборке 133804), только одиночная игра; Minecraft: Java Edition с
официальным лаунчером. BlockBreach использует Minecraft 26.3 + Fabric Loader 0.19.5 в своей папке игры.

## Установка
1. Запустите **BlockBreach.exe** — он сам найдёт Ready or Not и Minecraft Launcher.
2. **УСТАНОВИТЬ** — ставит всё (≈30 МБ), чужие файлы сохраняются в бэкап.
3. **ГОТОВ** — открывает Minecraft Launcher с выбранным профилем *BlockBreach*: нажмите там **PLAY**. Когда мир
   откроется, BlockBreach запустит Ready or Not в DirectX 11.
4. Начните любую миссию.

SmartScreen может предупредить о неизвестном издателе (exe не подписан): «Подробнее → Выполнить в любом случае».

## Управление
NumPad 0 — режим стройки; ЛКМ/ПКМ — ломать/ставить; 1–9 и колесо — слот; СКМ — взять блок; E — инвентарь (E/Esc —
закрыть); ваше оружие попадает по мобам; F5 — Minecraft в кадре вкл/выкл; F6 — выровнять пол; F7 — под/поверх HUD.

## Удаление
BlockBreach → **УСТАНОВКА → УДАЛИТЬ** (или «Приложения» Windows). Всё убирается, сохранённые файлы возвращаются.

## Если не работает
Ready or Not должна быть в **DirectX 11**; Minecraft должен открыть мир до начала миссии. Журналы:
`…\Win64\ue4ss\UE4SS.log`, `…\ue4ss\Mods\RoNPassthrough\history.log`, `…\Win64\ReShade.log`,
`%LOCALAPPDATA%\BlockBreach\minecraft\logs\latest.log`. После обновления RoN — УСТАНОВКА → ПОЧИНИТЬ.
