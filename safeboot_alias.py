# safeboot_alias.py - PIO pre-script for the _ottelo envs.
#
# post_esp32.py needs two files in the variants dir for the .factory.bin:
#  - <env name>-safeboot.bin  only for its check; otherwise it downloads that
#    name from ota.tasmota.com, which has no tasmota32c3_ottelo_tc-safeboot.bin
#    (since Tasmota 16 a failed download means no factory image),
#  - the board's flash_extra_images entry (e.g. tasmota32c6-safeboot.bin),
#    which is what gets merged at 0x10000.
# Both are the OFFICIAL safeboot here, downloaded once by its board name. Our
# overlays are of no use in safeboot, they only make it bigger (C6 did not fit).
# Do not build the *-safeboot envs: they would overwrite the official file.
Import("env")
import os
import shutil
import urllib.request

URL = "http://ota.tasmota.com/tasmota32/release/"


def valid(path):
    try:
        with open(path, "rb") as f:
            return f.read(1) == b"\xE9"
    except OSError:
        return False


def fetch(fname, path):
    try:
        with urllib.request.urlopen(URL + fname, timeout=30) as r:
            data = r.read()
    except Exception as e:
        print("safeboot_alias: download of " + fname + " failed: " + str(e))
        return False
    if data[:1] != b"\xE9":
        print("safeboot_alias: " + fname + " from the server is no firmware image")
        return False
    with open(path, "wb") as f:
        f.write(data)
    print("safeboot_alias: downloaded official " + fname + " (" + str(len(data)) + " bytes)")
    return True


pioenv = env["PIOENV"]
if "safeboot" not in pioenv and "_" in pioenv:
    vdir = env.BoardConfig().get("build.variants_dir", "")
    if not vdir:
        fw = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
        vdir = os.path.join(fw, "variants", "tasmota")
    if not os.path.isabs(vdir):   # Tasmota 16: variants/tasmota inside the project
        vdir = os.path.join(env.subst("$PROJECT_DIR"), vdir)
    os.makedirs(vdir, exist_ok=True)
    names = [os.path.basename(e[1]) for e in env.BoardConfig().get("upload.arduino.flash_extra_images", [])
             if len(e) > 1 and e[1].endswith("-safeboot.bin")]
    if not names:
        names = ["tasmota" + env.BoardConfig().get("build.mcu", "esp32")[3:] + "-safeboot.bin"]
    src = os.path.join(vdir, names[0])
    if valid(src) or fetch(names[0], src):
        shutil.copyfile(src, os.path.join(vdir, pioenv + "-safeboot.bin"))
        print("safeboot_alias: " + names[0] + " -> " + pioenv + "-safeboot.bin")
    else:
        print("safeboot_alias: no " + names[0] + " - no .factory.bin for " + pioenv)
