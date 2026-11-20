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
# for shipping: one app for Apple Silicon and Intel Macs, macOS 13 or newer (falls back to this Mac only if that fails)
MINOS=13.0; ARCHS="arm64 x86_64"
compile_all() {
  rm -f build/obj/*.o
  for f in $SRC; do
    echo "compile $f ($ARCHS, macOS $MINOS)" >> "$LOG/build.log"
    clang $CF $AF -c "$f" -o "build/obj/$(basename "$f" .c).o" >> "$LOG/build.log" 2>&1 || echo "FAILED $f" >> "$LOG/build.log"
  done
}
AF="-arch arm64 -arch x86_64 -mmacosx-version-min=$MINOS"; compile_all
if grep -q "^FAILED" "$LOG/build.log"; then
  echo "NOTE: universal build failed, building for this Mac only" >> "$LOG/build.log"; sed -i '' 's/^FAILED/failed (universal)/' "$LOG/build.log"
  ARCHS="$(uname -m)"; AF="-arch $ARCHS -mmacosx-version-min=$MINOS"; compile_all
fi
if grep -q "^FAILED" "$LOG/build.log"; then echo; echo "The build did not succeed (log/build.log). Tell Claude: build failed."; exit 1; fi

echo "Self-test ..."
CORE="build/obj/cm.o build/obj/part_chord.o build/obj/part_ext.o build/obj/chords.o build/obj/phrases.o build/obj/json.o build/obj/aura.o build/obj/titan.o build/obj/fathom.o"
clang $CF $AF -o build/cm-selftest selftest/core_test.c $CORE >> "$LOG/build.log" 2>&1
{ echo "== $(date)"; build/cm-selftest; } > "$LOG/selftest.txt" 2>&1
tail -1 "$LOG/selftest.txt" | sed 's/^/  /'

echo "Building Chordially.app ..."
APP="build/Chordially.app"; rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
PBV=$(/usr/libexec/PlistBuddy -c "Print CFBundleVersion" shell/Info.plist 2>/dev/null); CBV=$(grep -o "define CM_BUILD [0-9]*" core/cm.h | awk "{print \$3}"); [ "$PBV" = "$CBV" ] || echo "WARNING: Info.plist CFBundleVersion $PBV != CM_BUILD $CBV (core/cm.h)"
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
FW="-framework AudioToolbox -framework CoreAudio -framework CoreMIDI -framework AVFoundation -framework CoreAudioKit -framework WebKit -framework Cocoa"
status=0; SL=""
for a in $ARCHS; do
  swiftc -O -swift-version 5 -target $a-apple-macos$MINOS -import-objc-header shell/shell.h -o "build/Chordially-$a" shell/main.swift shell/insert.swift build/obj/*.o $FW >> "$LOG/build.log" 2>&1 \
    || { echo "swiftc $a failed" >> "$LOG/build.log"; status=1; }
  SL="$SL build/Chordially-$a"
done
if [ $status -ne 0 ]; then                              # (the old way: this Mac's own architecture and macOS)
  echo "NOTE: universal / macOS $MINOS app build failed, building for this Mac only" >> "$LOG/build.log"
  ARCHS="$(uname -m)"
  swiftc -O -swift-version 5 -import-objc-header shell/shell.h -o "$APP/Contents/MacOS/Chordially" shell/main.swift shell/insert.swift build/obj/*.o $FW >> "$LOG/build.log" 2>&1
  status=$?
else
  lipo -create $SL -output "$APP/Contents/MacOS/Chordially" >> "$LOG/build.log" 2>&1; status=$?
fi
echo "swiftc exit $status ($(lipo -archs "$APP/Contents/MacOS/Chordially" 2>/dev/null))" >> "$LOG/build.log"
if [ $status -ne 0 ]; then echo; echo "The app build did not succeed (log/build.log). Tell Claude: build failed."; exit 1; fi
codesign --force -s - "$APP" >> "$LOG/build.log" 2>&1

ROOT="$(cd .. && pwd)"; JA="$ROOT/Chordially.app"
if grep -q "ALL PASS" "$LOG/selftest.txt"; then
  rm -rf "$JA"; ditto "$APP" "$JA" && codesign --force -s - "$JA" >> "$LOG/build.log" 2>&1; touch "$JA"; RUN="$JA"
  /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$JA" > /dev/null 2>&1   # (new icon shown at once, not the cached one)
  VER=$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" "$JA/Contents/Info.plist" 2>/dev/null || echo 1.0)
  # the installer: a styled drag-to-Applications DMG (dmgbuild writes the Finder layout itself: no Finder prompt)
  DMG="$ROOT/Chordially.dmg"; rm -f "$DMG"
  VENV="build/dmgvenv"   # (dmgbuild in its own little Python environment: Homebrew's Python refuses "pip install --user")
  if [ ! -x "$VENV/bin/python3" ] || ! "$VENV/bin/python3" -c "import dmgbuild" 2>/dev/null; then
    echo "Installing dmgbuild (one time) ..."; rm -rf "$VENV"; python3 -m venv "$VENV" > "$LOG/dmgbuild.txt" 2>&1 && "$VENV/bin/python3" -m pip install --quiet --disable-pip-version-check dmgbuild >> "$LOG/dmgbuild.txt" 2>&1
  fi
  if "$VENV/bin/python3" -m dmgbuild -s res/dmg_settings.py -D app="$JA" -D bg="res/dmg_bg.png" -D icon="$JA/Contents/Resources/Chordially.icns" "Chordially $VER" "$DMG" >> "$LOG/dmgbuild.txt" 2>&1; then
    echo "Styled DMG installer built."
  else
    echo "dmgbuild did not work (log/dmgbuild.txt): plain DMG instead."
    STAGE="build/dmg"; rm -rf "$STAGE"; mkdir -p "$STAGE"; ditto "$JA" "$STAGE/Chordially.app"; ln -s /Applications "$STAGE/Applications"
    hdiutil create -volname "Chordially $VER" -srcfolder "$STAGE" -fs HFS+ -format UDZO -imagekey zlib-level=9 -ov "$DMG" >> "$LOG/build.log" 2>&1; rm -rf "$STAGE"
  fi
  if [ -f "$DMG" ] && { [ -n "$CHORDIALLY_DMG_BACKUP" ] || [ -d "$HOME/Claude Projects" ]; }; then BK="$HOME/Downloads/DMG-backup-$(date +%Y-%m-%d)"; mkdir -p "$BK" && cp "$DMG" "$BK/Chordially-$VER.dmg" && echo "Chordially-$VER.dmg copied to $BK"; fi   # (optional backup copy)
  # install: /Applications, a Desktop shortcut, and (for a developer copy) the data folder beside the project
  if [ -z "$CHORDIALLY_NO_INSTALL" ]; then
    pkill -x Chordially 2>/dev/null; sleep 0.5
    if ditto "$JA" "/Applications/Chordially.app.new" 2>/dev/null; then
      rm -rf "/Applications/Chordially.app" && mv "/Applications/Chordially.app.new" "/Applications/Chordially.app" && echo "Installed /Applications/Chordially.app"
      RUN="/Applications/Chordially.app"
      /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$RUN" > /dev/null 2>&1
      ln -sfn "$RUN" "$HOME/Desktop/Chordially" && echo "Desktop shortcut -> $RUN"
      AS="$HOME/Library/Application Support/Chordially"
      if [ -d "$ROOT/Chordially data" ] && [ ! -e "$AS" ]; then ln -s "$ROOT/Chordially data" "$AS" && echo "Data: $AS -> $ROOT/Chordially data"; fi
    else
      echo "Could not write /Applications: running from the project folder."
    fi
  fi
  echo "Chordially.app, Chordially.dmg (installer) ready; Chordially is in Applications and on the Desktop."
else
  echo "Self-test did not pass: Chordially.app NOT replaced; running the build from its folder. Tell Claude."; RUN="$APP"
fi
echo
echo "Running Chordially. Cmd+Q when done, then close this window."
CM_LOG="$LOG/app.log" "$RUN/Contents/MacOS/Chordially" > "$LOG/app-stdout.log" 2>&1
echo "app exit $?" >> "$LOG/app-stdout.log"
echo "Done."
