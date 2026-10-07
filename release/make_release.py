"""Builds BlockBreach's release files into dist/ from the current builds (run after make_payload.py + cargo build):

  python release/make_release.py

  dist/BlockBreach-<v>/                 BlockBreach.exe + README.md, CHANGELOG.md, LICENSE, THIRD_PARTY_NOTICES.md
  dist/BlockBreach-<v>.zip              the same, zipped (the main download)
  dist/BlockBreach-<v>-manual.zip       the mod laid out as it installs, for people who don't run installers:
                                        ReadyOrNot/Binaries/Win64/..., Minecraft/mods/..., Minecraft/versions/...
"""
import pathlib
import re
import shutil
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
REL = ROOT / "release"
DIST = ROOT / "dist"
VERSION = re.search(r'^version = "([^"]+)"', (ROOT / "installer" / "Cargo.toml").read_text(), re.M)[1]
EXE = ROOT / "installer" / "target" / "release" / "BlockBreach.exe"
PAYLOAD = ROOT / "installer" / "build" / "payload.zip"


def notices() -> str:
    parts = ["# Third-party notices\n",
             "BlockBreach ships or downloads these components; each keeps its own license.\n"]
    sections = [
        ("UE4SS (RE-UE4SS)", "MIT", (ROOT / "lab" / "payload" / "ue4ss" / "LICENSE").read_text(encoding="utf-8", errors="replace")),
        ("ReShade", "BSD 3-Clause", (ROOT / "src" / "reshade" / "LICENSE.md").read_text(encoding="utf-8", errors="replace")),
        ("Fabric Loader / Fabric API", "Apache License 2.0",
         "Fabric API is included as an unmodified jar; Fabric Loader is downloaded by the Minecraft Launcher from "
         "maven.fabricmc.net. Licensed under the Apache License, Version 2.0: https://www.apache.org/licenses/LICENSE-2.0"),
        ("mc-gta5-passthrough example (rehan)", "MIT", "The Minecraft passthrough mod and the compositor are derived from it."),
    ]
    for name, lic, text in sections:
        parts.append(f"\n## {name} — {lic}\n\n```\n{text.strip()}\n```\n")
    for font in ("Oswald", "RobotoCondensed", "JetBrainsMono", "PressStart2P"):
        text = (ROOT / "installer" / "assets" / f"OFL-{font}.txt").read_text(encoding="utf-8", errors="replace")
        parts.append(f"\n## {font} font — SIL Open Font License 1.1\n\n```\n{text.strip()}\n```\n")
    return "".join(parts)


def main():
    out = DIST / f"BlockBreach-{VERSION}"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    # the build machine's user folder out of the exe (Rust keeps source paths for panic messages); same length
    data = EXE.read_bytes()
    home = pathlib.Path.home().name.encode()
    for sep in (b"\\", b"/"):
        data = data.replace(b"Users" + sep + home + sep, b"Users" + sep + b"build"[: len(home)].ljust(len(home), b"_") + sep)
    (out / "BlockBreach.exe").write_bytes(data)
    for name in ("README.md", "CHANGELOG.md", "LICENSE"):
        shutil.copy2(REL / name, out / name)
    (out / "THIRD_PARTY_NOTICES.md").write_text(notices(), encoding="utf-8")
    main_zip = DIST / f"BlockBreach-{VERSION}.zip"
    with zipfile.ZipFile(main_zip, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in sorted(out.iterdir()):
            z.write(f, f"BlockBreach-{VERSION}/{f.name}")
    # the manual layout, from the installer's own payload
    manual = DIST / f"BlockBreach-{VERSION}-manual.zip"
    with zipfile.ZipFile(PAYLOAD) as src, zipfile.ZipFile(manual, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for info in src.infolist():
            data = src.read(info)
            if info.filename.startswith("ron/"):
                z.writestr("ReadyOrNot/Binaries/Win64/" + info.filename[4:], data)
            elif info.filename.startswith("mc/"):
                z.writestr("Minecraft/" + info.filename[3:], data)
        z.writestr("INSTALL_MANUALLY.txt", MANUAL)
        for name in ("README.md", "CHANGELOG.md", "LICENSE"):
            z.write(REL / name, name)
        z.writestr("THIRD_PARTY_NOTICES.md", notices())
    for f in (main_zip, manual):
        print(f"{f}: {f.stat().st_size / 1e6:.1f} MB")


MANUAL = """BlockBreach - manual install (the installer BlockBreach.exe does all of this for you)

1. Ready or Not: copy the contents of ReadyOrNot/Binaries/Win64 into
   <Steam>/steamapps/common/Ready Or Not/ReadyOrNot/Binaries/Win64
   (back up dwmapi.dll, dxgi.dll, ReShade.ini, ReShadePreset.ini and the ue4ss folder first if you have them).
2. Minecraft: copy Minecraft/versions/fabric-loader-0.19.5-26.3 into %APPDATA%/.minecraft/versions (create an empty
   fabric-loader-0.19.5-26.3.jar next to the json if the launcher complains), and make a new installation in the
   Minecraft Launcher: version "fabric-loader-0.19.5-26.3", its own game directory; put Minecraft/mods/*.jar into
   that directory's mods folder and Minecraft/options.txt into the directory itself.
3. Start Minecraft with that installation (it opens its world by itself), then Ready or Not in DirectX 11
   (Steam: Play Ready or Not (DirectX 11)).
Uninstall: delete what you copied (and ue4ss/Mods/RoNPassthrough/history.log, ReShade.log).
"""

if __name__ == "__main__":
    main()
