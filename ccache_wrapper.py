"""
ccache_wrapper.py — optionaler PIO-Pre-Script-Hook für ccache.

Aktiviert ccache als Compiler-Wrapper, falls auf dem System installiert.
Reduziert Compile-Zeit drastisch, wenn dieselbe Tasmota-Codebase
mehrfach mit leicht unterschiedlichen `-D`-Flags gebaut wird
(z.B. `_ottelo_tas` und `_ottelo_tc` direkt hintereinander).

Wie ccache hilft, obwohl jede env andere -D-Flags hat:
  ccache läuft im Preprocessor-Mode — es preprocessed den Source,
  hashed das Ergebnis, und liefert das gecachte .o, falls der
  preprocessed Output identisch ist. Files wie lwip/freertos/lvgl,
  die OTTELO_VARIANT_* gar nicht referenzieren, haben in beiden
  Builds dasselbe Preprocessor-Output → Cache-Hit.

Aktivierung (in platformio_tasmota_cenv.ini, pro env):
  extra_scripts = ${env:tasmota32_base.extra_scripts}
                  pre:ccache_wrapper.py

Setup pro Plattform:
  Linux / Gitpod:   apt-get install ccache
  macOS:            brew install ccache
  Windows:          choco install ccache    (oder via scoop, MSYS2)

Empfohlene ccache-Settings für Tasmota (einmalig setzen):
  ccache -M 4G
  ccache --set-config sloppiness=time_macros,pch_defines,include_file_mtime
"""

import shutil
import subprocess

Import("env")  # noqa: F821  (PIO-injected)

ccache = shutil.which("ccache")
if not ccache:
    print("[ccache_wrapper] ccache nicht gefunden — Build ohne Cache.")
    print("[ccache_wrapper]   Linux:   sudo apt-get install ccache")
    print("[ccache_wrapper]   macOS:   brew install ccache")
    print("[ccache_wrapper]   Windows: choco install ccache")
else:
    # SCons-Action-Strings patchen statt CC/CXX zu replacen:
    # - CCCOM   = "$CC ... $SOURCES"         → "ccache $CC ... $SOURCES"
    # - CXXCOM  = "$CXX ... $SOURCES"        → "ccache $CXX ... $SOURCES"
    # - SHCCCOM / SHCXXCOM analog (Shared-Lib-Variante)
    #
    # Vorteil ggü. env.Replace(CC=...): SCons akzeptiert "$CC" als Programm-
    # Variable mit reinem Pfad. Wenn wir CC zu "ccache /pfad/zu/gcc" machen,
    # zerlegt SCons das oft falsch — und PIO's Toolchain-Init überschreibt
    # CC nach dem Pre-Script wieder. CCCOM dagegen wird nur einmal beim
    # Builder-Setup gelesen und bleibt stabil.
    patched = []
    for var in ("CCCOM", "CXXCOM", "SHCCCOM", "SHCXXCOM"):
        cmd = env.get(var, "")
        if cmd and "ccache" not in cmd:
            env[var] = f"{ccache} {cmd}"
            patched.append(var)

    print(f"[ccache_wrapper] aktiv: {ccache}")
    if patched:
        print(f"[ccache_wrapper] patched: {', '.join(patched)}")
    else:
        print("[ccache_wrapper] WARN: keine Builder-Strings gefunden")

    # Cache-Stats kurz ausgeben (best-effort, scheitert nicht den Build).
    try:
        stats = subprocess.run(
            [ccache, "-s"], capture_output=True, text=True, timeout=2
        )
        size_line = next(
            (l for l in stats.stdout.splitlines() if "cache size" in l.lower()),
            None,
        )
        if size_line:
            print(f"[ccache_wrapper] {size_line.strip()}")
    except Exception:
        pass
