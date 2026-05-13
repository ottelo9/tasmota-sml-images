#!/bin/bash
# Erstellt die Release-ZIPs für die ottelo-Tasmota-Images.
# - prüft, ob die erwarteten Builds existieren
# - meldet fehlende Dateien (ZIP wird übersprungen, Script läuft weiter)
# - legt einen Output-Ordner an
# - erstellt pro Plattform getrennte ZIPs für die beiden Varianten:
#     _tas → klassischer Tasmota-Scripter
#     _tc  → TinyC VM + Browser-IDE (kein Scripter)
# - Ausnahmen: Berry (nur _tas), ESP8266 1M (nur _tas), ESP8266 4M (beide)
#
# Erwartetes Layout: alle .bin / .bin.gz / .factory.bin liegen im aktuellen
# Arbeitsverzeichnis (typisch nach `cp build_output/firmware/* .`).

set -euo pipefail

OUTPUT_DIR="tasmota_zips"

mkdir -p "$OUTPUT_DIR"

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

    if zip -9 "$OUTPUT_DIR/$zip_name" "${files[@]}"; then
        echo "Erstellt: $OUTPUT_DIR/$zip_name"
        return 0
    else
        echo "FEHLER: Erstellen von $zip_name fehlgeschlagen"
        return 2
    fi
}

# =============================================================================
# ESP8266 Bundle — alle ESP8266-Builds gesammelt in einem ZIP
#   1M Flash: nur Scripter (UFILESYS fehlt → kein TinyC)
#   4M Flash: Scripter + TinyC
# =============================================================================
create_zip "tasmota8266_bundle_ottelo.zip" \
    tasmota-minimal.bin.gz \
    tasmota1m_ottelo_tas.bin.gz \
    tasmota1m_energy_ottelo_tas.bin.gz \
    tasmota1m_shelly_ottelo_tas.bin.gz \
    tasmota4m_ottelo_tas.bin.gz \
    tasmota4m_ottelo_tc.bin.gz || true

# =============================================================================
# ESP32 + Berry — nur Scripter-Variante (kein TinyC, weil Berry-only)
# =============================================================================
create_zip "tasmota32berry_ottelo_tas.zip" \
    tasmota32berry_ottelo_tas.bin \
    tasmota32berry_ottelo_tas.factory.bin || true

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
)

for board in "${ESP32_BOARDS[@]}"; do
    for variant in tas tc; do
        create_zip "${board}_${variant}.zip" \
            "${board}_${variant}.bin" \
            "${board}_${variant}.factory.bin" || true
    done
done

echo ""
echo "Fertig! ZIP-Dateien (falls erfolgreich) liegen im Ordner: $OUTPUT_DIR"
