#!/bin/bash
# Chordially: build, self-test and run. Double-click this file. The logs go to "Chordially/log", the app to
# "Chordially/Chordially.app" and "Chordially.dmg" beside it (optionally a backup copy of the DMG in ~/Downloads/DMG-backup-<date>).
#  1. the build tools (tools.txt)  2. compile (build.log)  3. self-test (selftest.txt: must end ALL PASS)  4. app + run
cd "$(dirname "$0")" || exit 1
LOG=../log
mkdir -p "$LOG" build/obj
echo "Chordially: checking the build tools ..."
{ echo "== $(date)"; sw_vers; uname -m; xcrun --find clang && clang --version; xcrun --find swiftc && swiftc --version; } > "$LOG/tools.txt" 2>&1
if ! xcrun --find clang > /dev/null 2>&1 || ! xcrun --find swiftc > /dev/null 2>&1; then
  echo "Apple's Command Line Tools are missing: click Install in Apple's window, then double-click this file again."
  xcode-select --install; exit 1
fi
CF="-O2 -ffp-contract=off -w -Icore -Ishell -Iengines/classic1 -Iengines/classic2"
SRC="core/cm.c core/part_chord.c core/part_ext.c core/chords.c core/phrases.c core/json.c engines/classic1/aura.c engines/classic1/titan.c engines/classic2/fathom.c shell/shell.c shell/ain.c"
: > "$LOG/build.log"
echo "Compiling ..."
rm -f build/obj/*.o
for f in $SRC; do
  echo "compile $f" >> "$LOG/build.log"
  clang $CF -c "$f" -o "build/obj/$(basename "$f" .c).o" >> "$LOG/build.log" 2>&1 || echo "FAILED $f" >> "$LOG/build.log"
done
if grep -q "^FAILED" "$LOG/build.log"; then echo; echo "The build did not succeed (log/build.log). Tell Claude: build failed."; exit 1; fi

echo "Self-test ..."
CORE="build/obj/cm.o build/obj/part_chord.o build/obj/part_ext.o build/obj/chords.o build/obj/phrases.o build/obj/json.o build/obj/aura.o build/obj/titan.o build/obj/fathom.o"
clang $CF -o build/cm-selftest selftest/core_test.c $CORE >> "$LOG/build.log" 2>&1
{ echo "== $(date)"; build/cm-selftest; } > "$LOG/selftest.txt" 2>&1
tail -1 "$LOG/selftest.txt" | sed 's/^/  /'

echo "Building Chordially.app ..."
APP="build/Chordially.app"; rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp shell/Info.plist "$APP/Contents/Info.plist"
cp web/index.html "$APP/Contents/Resources/index.html"
if [ -f web/icon-1024.png ]; then                       # the icon: one PNG -> an .icns
  IS="build/Chordially.iconset"; rm -rf "$IS"; mkdir -p "$IS"
  for s in 16 32 128 256 512; do
    sips -z $s $s web/icon-1024.png --out "$IS/icon_${s}x${s}.png" > /dev/null 2>&1
    sips -z $((s*2)) $((s*2)) web/icon-1024.png --out "$IS/icon_${s}x${s}@2x.png" > /dev/null 2>&1
  done
  iconutil -c icns "$IS" -o "$APP/Contents/Resources/Chordially.icns" >> "$LOG/build.log" 2>&1
fi
swiftc -O -swift-version 5 -import-objc-header shell/shell.h -o "$APP/Contents/MacOS/Chordially" shell/main.swift shell/insert.swift build/obj/*.o \
  -framework AudioToolbox -framework CoreAudio -framework CoreMIDI -framework AVFoundation -framework CoreAudioKit -framework WebKit -framework Cocoa >> "$LOG/build.log" 2>&1
status=$?; echo "swiftc exit $status" >> "$LOG/build.log"
if [ $status -ne 0 ]; then echo; echo "The app build did not succeed (log/build.log). Tell Claude: build failed."; exit 1; fi
codesign --force -s - "$APP" >> "$LOG/build.log" 2>&1

ROOT="$(cd .. && pwd)"; JA="$ROOT/Chordially.app"
if grep -q "ALL PASS" "$LOG/selftest.txt"; then
  rm -rf "$JA"; ditto "$APP" "$JA" && codesign --force -s - "$JA" >> "$LOG/build.log" 2>&1; touch "$JA"; RUN="$JA"
  /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$JA" > /dev/null 2>&1   # (new icon shown at once, not the cached one)
  VER=$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" "$JA/Contents/Info.plist" 2>/dev/null || echo 1.0)
  STAGE="build/dmg"; rm -rf "$STAGE"; mkdir -p "$STAGE"; ditto "$JA" "$STAGE/Chordially.app"; ln -s /Applications "$STAGE/Applications"
  cp licenses/GPL-3.0.txt "$STAGE/LICENSE (GPL-3.0).txt" 2>/dev/null; cp ../NOTICE.md "$STAGE/NOTICE.md" 2>/dev/null
  rm -f "$ROOT/Chordially.dmg"
  if hdiutil create -volname "Chordially $VER" -srcfolder "$STAGE" -fs HFS+ -format UDZO -imagekey zlib-level=9 -ov "$ROOT/Chordially.dmg" >> "$LOG/build.log" 2>&1; then
    if [ -n "$CHORDIALLY_DMG_BACKUP" ] || [ -d "$HOME/Claude Projects" ]; then BK="$HOME/Downloads/DMG-backup-$(date +%Y-%m-%d)"; mkdir -p "$BK" && cp "$ROOT/Chordially.dmg" "$BK/Chordially-$VER.dmg" && echo "Chordially-$VER.dmg copied to $BK"; fi   # (optional backup copy)
  fi
  rm -rf "$STAGE"
  echo "Chordially.app and Chordially.dmg are ready in the Chordially folder."
else
  echo "Self-test did not pass: Chordially.app NOT replaced; running the build from its folder. Tell Claude."; RUN="$APP"
fi
echo
echo "Running Chordially. Cmd+Q when done, then close this window."
CM_LOG="$LOG/app.log" "$RUN/Contents/MacOS/Chordially" > "$LOG/app-stdout.log" 2>&1
echo "app exit $?" >> "$LOG/app-stdout.log"
echo "Done."
