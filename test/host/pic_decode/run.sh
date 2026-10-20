#!/bin/sh
# PC test for main/pic_decode.c (PNG/GIF). Needs gcc, zlib headers and Python 3 with Pillow.
# Builds with stand-ins (inc/): zlib for the ROM's tinfl, and a malloc that puts every block
# against a no-access page, so any out-of-bounds access crashes. Then checks the decoded
# pixels against Pillow and fuzzes with damaged files. Run from anywhere: test/host/pic_decode/run.sh
set -e
cd "$(dirname "$0")"
W=$(mktemp -d)
gcc -g -O1 -Wall -Iinc -I../../../main -o "$W/picdec" test.c ../../../main/pic_decode.c -lz
mkdir -p "$W/img" "$W/img16" "$W/fuzz"
python3 gen.py "$W/img"
python3 png16.py "$W/img16"
cp ref.py fuzz.py "$W/"
cd "$W"
for f in img/*.png img/*.gif img16/*.png; do
  case "$f" in *.ref.png|*grey16.png) continue;; esac
  for s in 0 1 3; do
    r=$(./picdec "$f" $s out.ppm) || true
    case "$r" in OK*) python3 ref.py "$f" $((1 << s)) out.ppm;; *) echo "$f step $((1 << s)): $r";; esac
  done
done
python3 fuzz.py 1 ${FUZZ:-3000}
echo "Work files: $W"
