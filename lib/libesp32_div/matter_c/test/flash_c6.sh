#!/usr/bin/env bash
# Reliable OTA flasher for the matter_c device-test C6, mirroring
# serial_monitor._flash_ota: serve firmware.bin on the LAN, trigger
# OtaUrl+Upgrade 1, KEEP the server alive through the whole download, and
# use safeboot-aware detection (Restart 1 recovery) rather than naive
# build-timestamp polling.
#
#   usage: flash_c6.sh [device_ip] [env]
#   env MYIP / PORT override autodetect.
set -uo pipefail
DEV=${1:-192.168.188.122}
ENVNAME=${2:-tinyc32c6-matter}
MYIP=${MYIP:-$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null)}
PORT=${PORT:-8099}
REPO="$(cd "$(dirname "$0")/../../../.." && pwd)"
BIN="$REPO/.pio/build/$ENVNAME/firmware.bin"
LOG=/tmp/flash_c6_httpd.log

[ -f "$BIN" ] || { echo "no $BIN — build '$ENVNAME' first"; exit 1; }
[ -n "$MYIP" ] || { echo "could not determine LAN IP (set MYIP=)"; exit 1; }

classify() {  # -> normal | safeboot | unreachable
  local r; r=$(curl -s -m 4 "http://$DEV/cm?cmnd=Status%202" 2>/dev/null)
  if echo "$r" | grep -q StatusFWR; then echo normal; return; fi
  if echo "$r" | grep -q '"Command":"Unknown"'; then echo safeboot; return; fi
  if curl -s -m 4 "http://$DEV/" 2>/dev/null | grep -qi safeboot; then echo safeboot; return; fi
  echo unreachable
}

( cd "$(dirname "$BIN")" && exec python3 -m http.server "$PORT" --bind "$MYIP" ) >"$LOG" 2>&1 &
HTTPD=$!; trap 'kill $HTTPD 2>/dev/null' EXIT; sleep 1
echo "serving firmware.bin ($(wc -c <"$BIN" | tr -d ' ') B) at http://$MYIP:$PORT/  -> $DEV"
curl -s -m 10 "http://$DEV/cm?cmnd=Backlog%20OtaUrl%20http://$MYIP:$PORT/firmware.bin%3B%20Upgrade%201" >/dev/null
echo "Upgrade triggered; device reboots into safeboot to pull the image..."

restarted=0
for i in $(seq 1 75); do
  sleep 4
  m=$(classify)
  case "$m" in
    normal)
      echo; echo "OK — app0 up after $((i*4))s"
      curl -s -m 5 "http://$DEV/cm?cmnd=Status%202" | head -c 320; echo
      exit 0;;
    safeboot)
      # only nudge once the download has actually completed (logged GET)
      if [ $restarted -eq 0 ] && grep -q "GET /firmware.bin" "$LOG" 2>/dev/null; then
        echo; echo "[download complete, device in safeboot -> Restart 1]"
        curl -s -m 6 "http://$DEV/cm?cmnd=Restart%201" >/dev/null
        restarted=1
      fi
      printf 's';;
    *) printf '.';;
  esac
done
echo; echo "timeout after 300s — check $DEV manually. httpd log:"; tail -3 "$LOG"
exit 2
