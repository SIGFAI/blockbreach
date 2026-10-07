"""Builds installer/build/payload.zip: everything BlockBreach installs, taken from the project's own builds.

  ron/  -> Ready or Not's Binaries\\Win64: UE4SS (our build) + the RoNPassthrough mod, ReShade (add-on build) + the
           compositor shader, minimal configs (only our mod enabled, no dump keys, no console).
  mc/   -> the Minecraft side: the Fabric Loader version JSON for 26.3 (the launcher fetches the libraries and the
           game itself), Fabric API + the passthrough mod, and options.txt for the BlockBreach game dir.
  licenses/ -> third-party licences shown on the About page.

Run: python installer/make_payload.py   (after scripts/build-host.bat and the Gradle build)
"""
import io
import pathlib
import re
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
LAB = ROOT / "lab" / "payload"
BIN = ROOT / "src" / "build"
MC_JAR = ROOT / "src" / "mc-gta5-passthrough-example" / "mc" / "build" / "libs" / "passthrough-0.1.0.jar"
FABRIC_API = next((pathlib.Path.home() / ".gradle" / "caches" / "modules-2" / "files-2.1" / "net.fabricmc.fabric-api" / "fabric-api"
                   / "0.161.0+26.3").rglob("fabric-api-0.161.0+26.3.jar"))
FABRIC_VERSION = "fabric-loader-0.19.5-26.3"
FABRIC_JSON = pathlib.Path.home() / "AppData" / "Roaming" / ".minecraft" / "versions" / FABRIC_VERSION / f"{FABRIC_VERSION}.json"
OUT = ROOT / "installer" / "build" / "payload.zip"


def scrub(data: bytes) -> bytes:
    """The build machine's user folder out of binaries (debug paths, panic messages): the same length, so nothing moves."""
    home = pathlib.Path.home().name.encode()
    for sep in (b"\\", b"/"):
        for users in (b"Users", b"users"):
            data = data.replace(users + sep + home + sep, users + sep + b"build"[: len(home)].ljust(len(home), b"_") + sep)
    return data


def settings_ini() -> str:
    text = (LAB / "ue4ss" / "UE4SS-settings.ini").read_text(encoding="utf-8")
    # no header/object dump hotkeys and no consoles for players
    for key, value in [("EnableDumping", "0"), ("ConsoleEnabled", "0"), ("GuiConsoleEnabled", "0"), ("GuiConsoleVisible", "0")]:
        text, n = re.subn(rf"(?m)^{key}\s*=.*$", f"{key} = {value}", text)
        assert n == 1, key
    return text


def mc_options() -> str:
    lines = (ROOT / "mcgame" / "options.txt").read_text(encoding="utf-8").splitlines()
    # Minecraft's own language follows the system (it's never seen); everything else as tested
    return "\n".join(l for l in lines if not l.startswith("lang:") and not l.startswith("lastServer:")) + "\n"


def main() -> int:
    files: dict[str, bytes] = {}
    files["ron/dwmapi.dll"] = (LAB / "dwmapi.dll").read_bytes()
    files["ron/dxgi.dll"] = (LAB / "dxgi.dll").read_bytes()
    files["ron/ReShade.ini"] = (LAB / "ReShade.ini").read_bytes()
    files["ron/ReShadePreset.ini"] = b"Techniques=MCPassthrough@MCPassthrough.fx\r\nTechniqueSorting=MCPassthrough@MCPassthrough.fx\r\n"
    files["ron/reshade-shaders/Shaders/MCPassthrough.fx"] = (ROOT / "src" / "ron-host" / "shaders" / "MCPassthrough.fx").read_bytes()
    for name in ("ReShade.fxh", "ReShadeUI.fxh"):
        files[f"ron/reshade-shaders/Shaders/{name}"] = (LAB / "reshade-shaders" / "Shaders" / name).read_bytes()
    files["ron/ue4ss/UE4SS.dll"] = (BIN / "Game__Shipping__Win64" / "bin" / "UE4SS.dll").read_bytes()
    files["ron/ue4ss/UE4SS-settings.ini"] = settings_ini().encode("utf-8")
    files["ron/ue4ss/LICENSE"] = (LAB / "ue4ss" / "LICENSE").read_bytes()
    files["ron/ue4ss/Mods/mods.txt"] = b"RoNPassthrough : 1\r\n"
    files["ron/ue4ss/Mods/RoNPassthrough/dlls/main.dll"] = (BIN / "ron-host" / "RoNPassthrough.dll").read_bytes()

    files[f"mc/versions/{FABRIC_VERSION}/{FABRIC_VERSION}.json"] = FABRIC_JSON.read_bytes()
    files[f"mc/mods/{FABRIC_API.name}"] = FABRIC_API.read_bytes()
    files["mc/mods/blockbreach-passthrough.jar"] = MC_JAR.read_bytes()
    files["mc/options.txt"] = mc_options().encode("utf-8")

    files["licenses/UE4SS.txt"] = (LAB / "ue4ss" / "LICENSE").read_bytes()
    assets = ROOT / "installer" / "assets"
    for name in ("PressStart2P", "Oswald", "RobotoCondensed", "JetBrainsMono"):
        files[f"licenses/{name}-OFL.txt"] = (assets / f"OFL-{name}.txt").read_bytes()

    files = {name: scrub(data) if name.endswith(".dll") else data for name, data in files.items()}
    OUT.parent.mkdir(parents=True, exist_ok=True)
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, data in sorted(files.items()):
            z.writestr(name, data)
    OUT.write_bytes(buffer.getvalue())
    total = sum(len(d) for d in files.values())
    print(f"{OUT}: {len(files)} files, {total / 1e6:.1f} MB -> {OUT.stat().st_size / 1e6:.1f} MB")
    for name, data in sorted(files.items()):
        print(f"  {len(data):>10}  {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
