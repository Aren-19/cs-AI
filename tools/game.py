"""Where Counter-Strike: Source is installed.

In order: the CSAI_GAME environment variable, the folder named in
data/game.txt, the Steam libraries Steam knows about, then Steam's default
folder. tools/game.ps1 does the same for the PowerShell side.
"""

import glob
import os
import re

DEFAULT = r"C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Source"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def _ok(path):
    return bool(path) and os.path.isdir(os.path.join(path, "cstrike"))

def _steam_folders():
    try:
        import winreg
    except ImportError:
        return
    for hive, key in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam"),
                      (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam")):
        try:
            with winreg.OpenKey(hive, key) as k:
                for name in ("SteamPath", "InstallPath"):
                    try:
                        yield os.path.normpath(winreg.QueryValueEx(k, name)[0])
                    except OSError:
                        pass
        except OSError:
            pass

def _libraries(steam):
    yield steam
    try:
        with open(os.path.join(steam, "steamapps", "libraryfolders.vdf"),
                  encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return
    for m in re.finditer(r'"path"\s+"([^"]+)"', text):
        yield os.path.normpath(m.group(1).replace("\\\\", "\\"))

def _named():
    """The folder in data/game.txt, whatever editor or shell wrote it."""
    try:
        with open(os.path.join(ROOT, "data", "game.txt"), "rb") as fh:
            raw = fh.read()
    except OSError:
        return None
    if raw.startswith((b"\xff\xfe", b"\xfe\xff")):          # PowerShell 5.1 ">" writes UTF-16
        text = raw.decode("utf-16", errors="replace")
    else:
        try:
            text = raw.decode("utf-8-sig")
        except UnicodeDecodeError:
            text = raw.decode("mbcs" if os.name == "nt" else "latin-1", errors="replace")
    return text.strip().strip('"').strip()

def find_game():
    env = os.environ.get("CSAI_GAME")
    if _ok(env):
        return env
    named = _named()
    if _ok(named):
        return named
    for steam in _steam_folders():
        for lib in _libraries(steam):
            p = os.path.join(lib, "steamapps", "common", "Counter-Strike Source")
            if _ok(p):
                return p
    return DEFAULT

GAME = find_game()
CSTRIKE = os.path.join(GAME, "cstrike")
SMDATA = os.path.join(CSTRIKE, "addons", "sourcemod", "data")
DATA = os.path.join(SMDATA, "csai")

def map_dirs():
    """Where the game finds map files, in the order it looks (gameinfo.txt
    mounts cstrike/custom/* before cstrike itself, and downloads last)."""
    custom = sorted(glob.glob(os.path.join(CSTRIKE, "custom", "*", "maps")), key=str.lower)
    return custom + [os.path.join(CSTRIKE, "maps"), os.path.join(CSTRIKE, "download", "maps")]

def find_bsp(map_name):
    """The map file the game would load, or None."""
    name = os.path.basename(map_name)
    for d in map_dirs():
        p = os.path.join(d, name + ".bsp")
        if os.path.isfile(p):
            return p
    return None

if __name__ == "__main__":
    print(GAME)
