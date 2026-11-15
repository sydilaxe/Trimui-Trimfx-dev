#!/bin/sh
# The self-update end to end, without the internet (make update-test; CI runs it). As in
# Conduit, Columbia Street, RadioScan and Jaffedrone.
#
# tools/fake_github.py serves a v9.9.9 release the way GitHub does: the API's JSON, a
# ~1000-character signed redirect, then the MustardOS package (.muxapp, a zip).
# build/trimfx_update installs it over an older app folder, as the app does when you press
# A. First, the same release with a damaged zip must fail and leave the folder as it was.
set -eu

T=build/update-test
PORT=${PORT:-8767}
rm -rf $T
mkdir -p "$T/rel/TrimFX/glyph" "$T/rel/TrimFX/presets" $T/bad $T/app/presets

# the release: launch.sh, mux_launch.sh and friends, a binary, the README, the glyph in its
# folder, and a state, MIDI map and preset slot that must never reach the user's folder
R="$T/rel/TrimFX"
cp Apps/TrimFX/launch.sh Apps/TrimFX/cacert.pem Apps/TrimFX/presets/README.txt mustardos/mux_launch.sh "$R/"
mv "$R/README.txt" "$R/presets/README.txt"
cp mustardos/glyph/trimfx.svg "$R/glyph/"
cp build/trimfx_update "$R/trimfx"
head -c 300000 /dev/urandom > "$R/README.md"
echo theirs > "$R/state.txt"
echo theirs > "$R/midimap.txt"
echo theirs > "$R/presets/slot01.txt"
Z=TrimFX-mustardos.muxapp
(cd $T/rel && zip -qr $Z "TrimFX")

# the device's folder: an older version, the user's state, MIDI map, presets and log, and a
# README that's already the same as the release's (dated 2001, so we can tell it's left alone)
A=$T/app
cp Apps/TrimFX/cacert.pem $A/
echo old > $A/mux_launch.sh
echo old > $A/trimfx
echo old > $A/launch.sh
echo old > $A/presets/README.txt
echo mine > $A/state.txt
echo mine > $A/midimap.txt
echo mine > $A/presets/slot01.txt
echo mine > $A/presets/slot16.txt
echo mine > $A/log.txt
cp "$R/README.md" $A/
touch -t 200101010000 $A/README.md

# the same release with its zipped binary damaged (the CRC check must catch it)
cp $T/rel/$Z $T/bad/
off=$(python3 -c '
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1]); i = z.getinfo("TrimFX/trimfx")
print(i.header_offset + 30 + len(i.filename) + len(i.extra) + i.compress_size // 2)' $T/bad/$Z)
printf 'XXXXXXXX' | dd of=$T/bad/$Z bs=1 seek="$off" conv=notrunc 2>/dev/null

python3 tools/fake_github.py $T/rel --port $PORT --tag v9.9.9 --asset $Z &
good=$!
python3 tools/fake_github.py $T/bad --port $((PORT + 1)) --tag v9.9.9 --asset $Z &
bad=$!
trap 'kill $good $bad 2>/dev/null || true' EXIT
for i in $(seq 1 50); do
    curl -s -o /dev/null http://127.0.0.1:$PORT/latest.json && curl -s -o /dev/null http://127.0.0.1:$((PORT + 1))/latest.json && break
    sleep 0.1
done

fail() { echo "update test: $*" >&2; exit 1; }

cp -r $A $T/before
if TFX_UPDATE_API=http://127.0.0.1:$((PORT + 1))/latest.json build/trimfx_update $A; then
    fail "a damaged zip was installed"
fi
diff -r $T/before $A >/dev/null || fail "a failed update changed the app folder"
echo "update test: a damaged zip is refused and changes nothing"

TFX_UPDATE_API=http://127.0.0.1:$PORT/latest.json build/trimfx_update $A
cmp -s $A/trimfx "$R/trimfx" || fail "the binary wasn't replaced"
test -x $A/trimfx || fail "the binary isn't executable"
cmp -s $A/launch.sh "$R/launch.sh" || fail "launch.sh wasn't replaced"
cmp -s $A/mux_launch.sh "$R/mux_launch.sh" || fail "mux_launch.sh wasn't replaced"
test -x $A/mux_launch.sh || fail "mux_launch.sh isn't executable"
cmp -s $A/presets/README.txt "$R/presets/README.txt" || fail "presets/README.txt wasn't replaced"
cmp -s $A/glyph/trimfx.svg "$R/glyph/trimfx.svg" || fail "the glyph's folder wasn't made"
for f in state.txt midimap.txt presets/slot01.txt presets/slot16.txt log.txt; do
    test "$(cat $A/$f)" = mine || fail "the user's $f was overwritten"
done
test "$(date -r $A/README.md +%Y)" = 2001 || fail "an unchanged file was rewritten"
test -z "$(find $A -name '*.new')" || fail ".new files were left behind"
echo "update test: installed v9.9.9 (the .muxapp) over an older version, kept the state, MIDI map, presets and log, skipped the unchanged README"
