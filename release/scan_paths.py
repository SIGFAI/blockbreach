"""Looks for the build machine's user paths and name inside the release binaries (they leak through debug info
and panic messages): python release/scan_paths.py <files...>"""
import pathlib
import sys
import zipfile

_ME = pathlib.Path.home().name.encode()  # the build machine's user name, as it shows in paths
NEEDLES = [b"Users\\" + _ME, b"Users/" + _ME, b"users\\" + _ME]


def scan(name, data):
    hits = {n.decode(): data.count(n) for n in NEEDLES if data.count(n)}
    if hits:
        i = data.find(next(n for n in NEEDLES if data.count(n)))
        print(f"{name}: {hits}  e.g. {data[max(0, i - 60):i + 80]!r}")
    return bool(hits)


found = False
for path in sys.argv[1:]:
    if path.endswith(".zip"):
        with zipfile.ZipFile(path) as z:
            for n in z.namelist():
                found |= scan(f"{path}:{n}", z.read(n))
    else:
        found |= scan(path, open(path, "rb").read())
print("found" if found else "clean")
