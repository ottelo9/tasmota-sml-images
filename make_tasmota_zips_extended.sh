#!/bin/bash
# Erstellt die Release-ZIPs für die ottelo-Tasmota-Images.
# - prüft, ob die erwarteten Builds existieren
# - meldet fehlende Dateien (ZIP wird übersprungen, Script läuft weiter)
# - legt einen Output-Ordner an
# - erstellt pro Plattform getrennte ZIPs für die beiden Varianten:
#     _tas → klassischer Tasmota-Scripter
#     _tc  → TinyC VM + Browser-IDE (kein Scripter)
# - ESP8266 (1M alle Varianten + 4M): nur _tas. TinyC (_tc) nur auf ESP32.
# - zippt nur neu, wenn eine Image-Datei neuer ist als das vorhandene ZIP;
#   ein veraltetes ZIP wird vorher geloescht (keine Altlasten im Archiv)
#
# Erwartetes Layout: alle .bin / .bin.gz / .factory.bin liegen im aktuellen
# Arbeitsverzeichnis (typisch nach `cp build_output/firmware/* .`).

set -euo pipefail

OUTPUT_DIR="tasmota_zips"

mkdir -p "$OUTPUT_DIR"

# --- Gibt 0 zurueck, wenn das ZIP schon existiert und keine der Dateien neuer
#     ist - dann bleibt es, wie es ist. Sonst wird ein vorhandenes ZIP geloescht,
#     damit es neu und ohne Altlasten (z.B. fruehere safeboot) entsteht. ---
zip_is_current() {
    local zip_path="$OUTPUT_DIR/$1"
    shift
    [ -f "$zip_path" ] || return 1
    for f in "$@"; do
        if [ "$f" -nt "$zip_path" ]; then
            rm -f "$zip_path"
            return 1
        fi
    done
    return 0
}

# --- Hilfsfunktion: ZIP nur erstellen, wenn alle Quellen existieren ---
create_zip() {
    local zip_name="$1"
    shift
    local files=("$@")

    echo ""
    echo "=== Erstelle $zip_name ==="

    local missing=false
    for f in "${files[@]}"; do
        if [ ! -f "$f" ]; then
            echo "FEHLER: Datei fehlt: $f"
            missing=true
        fi
    done

    if [ "$missing" = true ]; then
        echo "ZIP wird NICHT erstellt: $zip_name"
        return 1
    fi

    if zip_is_current "$zip_name" "${files[@]}"; then
        echo "Unveraendert, nicht neu gezippt: $OUTPUT_DIR/$zip_name"
        return 0
    fi

    if zip -9 "$OUTPUT_DIR/$zip_name" "${files[@]}"; then
        echo "Erstellt: $OUTPUT_DIR/$zip_name"
        return 0
    else
        echo "FEHLER: Erstellen von $zip_name fehlgeschlagen"
        return 2
    fi
}

# --- Toleranter ZIP-Helfer: packt alle VORHANDENEN Dateien, ueberspringt
#     einzeln fehlende (nur Warnung). Fuers Bundle sinnvoll, damit eine fehlende
#     .bin nicht das ganze Archiv verhindert. ---
create_zip_optional() {
    local zip_name="$1"
    shift
    local present=()

    echo ""
    echo "=== Erstelle $zip_name (tolerant) ==="

    for f in "$@"; do
        if [ -f "$f" ]; then
            present+=("$f")
        else
            echo "HINWEIS: uebersprungen (fehlt): $f"
        fi
    done

    if [ "${#present[@]}" -eq 0 ]; then
        echo "ZIP wird NICHT erstellt (keine Datei vorhanden): $zip_name"
        return 1
    fi

    if zip_is_current "$zip_name" "${present[@]}"; then
        echo "Unveraendert, nicht neu gezippt: $OUTPUT_DIR/$zip_name"
        return 0
    fi

    if zip -9 "$OUTPUT_DIR/$zip_name" "${present[@]}"; then
        echo "Erstellt: $OUTPUT_DIR/$zip_name  (${#present[@]} Dateien)"
        return 0
    else
        echo "FEHLER: Erstellen von $zip_name fehlgeschlagen"
        return 2
    fi
}

# =============================================================================
# ESP8266 Bundle — alle ESP8266-Builds gesammelt in einem ZIP
#   1M Flash: nur Scripter (UFILESYS fehlt → kein TinyC)
#   4M Flash: nur Scripter (kein TinyC — kein I2C fuer BinPlugin-Loader)
#   Nur .bin.gz (OTA "Use file upload"), keine .bin.
# =============================================================================
create_zip_optional "tasmota8266_bundle_ottelo.zip" \
    tasmota-minimal.bin.gz \
    tasmota1m_ottelo_tas.bin.gz \
    tasmota1m_energy_ottelo_tas.bin.gz \
    tasmota1m_shelly_ottelo_tas.bin.gz \
    tasmota1m_ct002_ottelo_tas.bin.gz \
    tasmota4m_ottelo_tas.bin.gz || true

# =============================================================================
# ESP32-Plattformen — je ein ZIP pro Variante (_tas und _tc)
# =============================================================================
ESP32_BOARDS=(
    "tasmota32_ottelo"          # Generic ESP32
    "tasmota32c3_ottelo"        # ESP32-C3
    "tasmota32c6_ottelo"        # ESP32-C6
    "tasmota32s2_ottelo"        # ESP32-S2
    "tasmota32s3_ottelo"        # ESP32-S3
    "tasmota32solo1_ottelo"     # ESP32-SOLO1
    "tasmota32p4_ottelo"        # ESP32-P4
    "tasmota32p4r3_ottelo"      # ESP32-P4 Rev.3
)

# safeboot: Tasmota baut eine pro Chip (tasmota32c3-safeboot.bin, ...). Sie wird
# je Variante unter dem Namen des Images abgelegt, z.B.
# tasmota32_ottelo_tc-safeboot.bin (für ota_firmware/, Safeboot-Update per URL),
# kommt aber NICHT ins ZIP - die .factory.bin enthält sie schon.
for board in "${ESP32_BOARDS[@]}"; do
    sb_src="${board%_ottelo}-safeboot.bin"
    for variant in tas tc; do
        files=("${board}_${variant}.bin" "${board}_${variant}.factory.bin")
        sb="${board}_${variant}-safeboot.bin"
        if [ -f "$sb_src" ]; then
            cp -f "$sb_src" "$sb"
        else
            echo "HINWEIS: $sb_src fehlt - kein $sb fuer ota_firmware/"
        fi
        create_zip "${board}_${variant}.zip" "${files[@]}" || true
    done
done

# =============================================================================
# DFR1154-Kamera (Env tasmota32s3_dfr1154) — eigenes ZIP, safeboot vom S3
# (nur als Datei für ota_firmware/, nicht im ZIP).
# Ihre OTA_URL zeigt auf ota_firmware/ESP32/tasmota32s3_dfr1154.bin.
# =============================================================================
files=("tasmota32s3_dfr1154.bin" "tasmota32s3_dfr1154.factory.bin")
if [ -f "tasmota32s3-safeboot.bin" ]; then
    cp -f "tasmota32s3-safeboot.bin" "tasmota32s3_dfr1154-safeboot.bin"
else
    echo "HINWEIS: tasmota32s3-safeboot.bin fehlt - kein tasmota32s3_dfr1154-safeboot.bin"
fi
create_zip "tasmota32s3_dfr1154.zip" "${files[@]}" || true

echo ""
echo "Fertig! ZIP-Dateien (falls erfolgreich) liegen im Ordner: $OUTPUT_DIR"
