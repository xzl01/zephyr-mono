#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build a local PaperMono HAL driver from BlackHole v0.7.1 (GPL-3.0).
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUILD="$HERE/build"
SRC="$BUILD/blackhole-src"
BUNDLE="$BUILD/PaperMono.driver"
BLACKHOLE_TAG="v0.7.1"
BLACKHOLE_URL="https://github.com/ExistentialAudio/BlackHole/archive/refs/tags/${BLACKHOLE_TAG}.tar.gz"
FACTORY_UUID="7c2d3a1e-9f4b-4a6c-8d2e-1b5f0a3c9d7e"
BUNDLE_ID="com.papermono.audio"

for tool in clang codesign curl tar; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "error: $tool is required" >&2
		exit 1
	fi
done

rm -rf "$BUNDLE"
mkdir -p "$BUILD"

if [ ! -f "$SRC/BlackHole/BlackHole.c" ]; then
	echo "fetching BlackHole $BLACKHOLE_TAG source..."
	rm -rf "$SRC"
	mkdir -p "$SRC"
	curl -fsSL "$BLACKHOLE_URL" | tar xz -C "$SRC" --strip-components=1
fi

mkdir -p "$BUNDLE/Contents/MacOS" "$BUNDLE/Contents/Resources"

echo "compiling PaperMono driver..."
clang -bundle -O2 -std=gnu11 -fPIC -mmacosx-version-min=11.0 \
	-DDEBUG=0 \
	-DkDriver_Name='"PaperMono"' \
	-DkHas_Driver_Name_Format=false \
	-DkDevice_Name='"PaperMono"' \
	-DkDevice2_Name='"PaperMono Mirror"' \
	-DkPlugIn_BundleID="\"$BUNDLE_ID\"" \
	-DkPlugIn_Icon='""' \
	-DkManufacturer_Name='"PaperMono"' \
	"$SRC/BlackHole/BlackHole.c" \
	-o "$BUNDLE/Contents/MacOS/PaperMono" \
	-framework CoreAudio -framework AudioToolbox \
	-framework CoreFoundation -framework Accelerate

cat > "$BUNDLE/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key>
	<string>English</string>
	<key>CFBundleExecutable</key>
	<string>PaperMono</string>
	<key>CFBundleIdentifier</key>
	<string>${BUNDLE_ID}</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleName</key>
	<string>PaperMono</string>
	<key>CFBundleDisplayName</key>
	<string>PaperMono</string>
	<key>CFBundlePackageType</key>
	<string>BNDL</string>
	<key>CFBundleShortVersionString</key>
	<string>1.0.0</string>
	<key>CFBundleVersion</key>
	<string>1</string>
	<key>CFBundleSignature</key>
	<string>????</string>
	<key>LSMinimumSystemVersion</key>
	<string>11.0</string>
	<key>CFPlugInFactories</key>
	<dict>
		<key>${FACTORY_UUID}</key>
		<string>BlackHole_Create</string>
	</dict>
	<key>CFPlugInTypes</key>
	<dict>
		<key>443ABAB8-E7B3-491A-B985-BEB9187030DB</key>
		<array>
			<string>${FACTORY_UUID}</string>
		</array>
	</dict>
</dict>
</plist>
PLIST

# Local development build: ad-hoc signature. Distributing this binary requires
# a Developer ID signature and notarization.
codesign --force --deep --sign - "$BUNDLE"
codesign --verify --deep --strict "$BUNDLE"

echo "built: $BUNDLE"
echo "install with: $HERE/install-papermono-driver.sh"
