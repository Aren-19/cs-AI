"""Where Counter-Strike: Source is installed.

In order: the CSAI_GAME environment variable, the folder named in
data/game.txt, the Steam libraries Steam knows about, then Steam's default
folder. tools/game.ps1 does the same for the PowerShell side.
"""

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

def find_game():
    env = os.environ.get("CSAI_GAME")
    if _ok(env):
        return env
    try:
        with open(os.path.join(ROOT, "data", "game.txt"), encoding="utf-8-sig") as fh:
            named = fh.read().strip()
        if _ok(named):
            return named
    except OSError:
        pass
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

if __name__ == "__main__":
    print(GAME)
