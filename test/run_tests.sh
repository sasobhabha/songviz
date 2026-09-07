#!/usr/bin/env bash
# songviz smoke tests — needs ffmpeg on PATH.
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0 fail=0
ok()   { echo "  ok: $1"; pass=$((pass+1)); }
fail_() { echo "  FAIL: $1"; fail=$((fail+1)); }

echo "== songviz smoke tests =="

command -v ffmpeg >/dev/null 2>&1 || { echo "ffmpeg required"; exit 1; }
[[ -x src/render ]] || make -s all || { echo "build failed"; exit 1; }

# --- test tone: 8 s, 440 Hz sine + second harmonic
ffmpeg -y -v error -f lavfi -i "sine=frequency=440:duration=8" \
       -f lavfi -i "sine=frequency=880:duration=8" \
       -filter_complex "[0:a][1:a]amix=inputs=2" "$TMP/tone.wav"

# --- version / help
"$ROOT/bin/songviz" --version >/dev/null && ok "--version" || fail_ "--version"
"$ROOT/bin/songviz" --help >/dev/null && ok "--help" || fail_ "--help"

# --- default landscape render (small, 2 s)
"$ROOT/bin/songviz" --width 640 --height 360 --seconds 2 \
    -t "Test Song" -s "songviz smoke test" --palette neon \
    "$TMP/tone.wav" "$TMP/out1.mp4" 2>"$TMP/log1" \
  && ok "landscape render" || { fail_ "landscape render"; cat "$TMP/log1"; }

# --- vertical + ocean palette
"$ROOT/bin/songviz" --vertical --width 360 --height 640 --seconds 2 \
    --palette ocean --fps 24 "$TMP/tone.wav" "$TMP/out2.mp4" 2>"$TMP/log2" \
  && ok "vertical render" || { fail_ "vertical render"; cat "$TMP/log2"; }

# --- square + matrix + custom bars
"$ROOT/bin/songviz" --square --width 480 --height 480 --seconds 2 \
    --palette matrix --bars 32 --gain 1.5 "$TMP/tone.wav" "$TMP/out3.mp4" 2>"$TMP/log3" \
  && ok "square render" || { fail_ "square render"; cat "$TMP/log3"; }

# --- verify dimensions and duration with ffprobe
if command -v ffprobe >/dev/null 2>&1; then
  probe() { ffprobe -v error -select_streams v:0 \
            -show_entries stream=width,height -show_entries format=duration \
            -of csv=p=0 "$1" 2>/dev/null | tr '\n' ' '; }

  dims=$(probe "$TMP/out1.mp4")
  [[ "$dims" == 640,360* ]] && ok "landscape dims (got: $dims)" || fail_ "landscape dims (got: $dims)"
  dur=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$TMP/out1.mp4" 2>/dev/null)
  awk -v d="$dur" 'BEGIN { exit !(d >= 1.0 && d <= 3.5) }' \
    && ok "duration ~2s ($dur)" || fail_ "duration (got: $dur)"

  dims=$(probe "$TMP/out2.mp4")
  [[ "$dims" == 360,640* ]] && ok "vertical dims" || fail_ "vertical dims (got: $dims)"

  dims=$(probe "$TMP/out3.mp4")
  [[ "$dims" == 480,480* ]] && ok "square dims" || fail_ "square dims (got: $dims)"

  # --- audio stream present and not silent for the whole file?
  astat=$(ffmpeg -i "$TMP/out1.mp4" -map 0:a -af volumedetect -f null - 2>&1 \
          | grep -o 'mean_volume: [-0-9.]*' | head -1)
  [[ "$astat" == *"-"* ]] && ok "audio stream present ($astat)" || fail_ "audio stream ($astat)"
fi

# --- missing input should fail cleanly
"$ROOT/bin/songviz" "$TMP/nope.mp3" "$TMP/no.mp4" 2>/dev/null \
  && fail_ "missing input should fail" || ok "missing input rejected"

# --- unknown option should fail cleanly
"$ROOT/bin/songviz" --bogus "$TMP/tone.wav" "$TMP/no.mp4" 2>/dev/null \
  && fail_ "unknown option should fail" || ok "unknown option rejected"

echo
echo "passed: $pass  failed: $fail"
[[ $fail -eq 0 ]]
