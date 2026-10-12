#!/bin/bash
# Build the app (Debug, the configuration that runs games; docs/BUILDING.md
# step 6) without code signing and package it as an IPA. The sideloader
# (AltStore, SideStore, ...) signs it at install time, as with the releases.
# Usage: ci/package-ipa.sh <output.ipa>
set -euo pipefail
R="$(cd "$(dirname "$0")/.." && pwd)"
IPA="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
DD="$R/build/xcode-derived"

# Folder references the project expects; WoW64 (i386-windows) is not built here
# yet, and x86_64-vcruntime is optional (tools/fetch-vcruntime.md).
mkdir -p "$R/app/Madeira/i386-windows" "$R/app/Madeira/x86_64-vcruntime"
# The generated licence copies (gitignored); the build fails without them.
bash "$R/build/stage-licenses.sh"

xcodebuild -project "$R/app/Madeira.xcodeproj" -scheme Madeira -configuration Debug \
    -destination 'generic/platform=iOS' -derivedDataPath "$DD" \
    CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY= \
    build

APP="$DD/Build/Products/Debug-iphoneos/Madeira.app"
STAGE="$(mktemp -d)"
mkdir "$STAGE/Payload"
ditto "$APP" "$STAGE/Payload/Madeira.app"
# Release packages leave Wine Mono out; the app downloads it on first use.
rm -rf "$STAGE/Payload/Madeira.app/wine-mono"

# Ad-hoc sign with the app's entitlements. The sideloader re-signs at install time and
# keeps the entitlements it finds in the existing signature (filtered by the profile);
# an unsigned build carries none, so an install could come up without
# increased-memory-limit (~3.3 GB instead of ~8 GB available on an iPhone 17 Pro) and
# jetsam ended Stellar Blade while it loaded. Nested code first, without entitlements,
# then the app with the iOS keys of Madeira.entitlements (allow-jit is a macOS key; a
# sideloader that does not filter by profile could refuse the install over it). A
# failure here leaves the IPA as before (unsigned) rather than failing the build.
adhoc_sign() {
    local A="$STAGE/Payload/Madeira.app" E="$STAGE/adhoc.entitlements" x
    cp "$R/app/Madeira/Madeira.entitlements" "$E"
    /usr/libexec/PlistBuddy -c 'Delete :com.apple.security.cs.allow-jit' "$E" 2>/dev/null || true
    while IFS= read -r -d '' x; do
        codesign --force --sign - --timestamp=none "$x" || return 1
    done < <(find "$A" -depth -mindepth 1 \( -name '*.appex' -o -name '*.framework' -o -name '*.dylib' \) -print0)
    codesign --force --sign - --timestamp=none --entitlements "$E" "$A" || return 1
    echo "Ad-hoc signed; entitlements the sideloader will see:"
    codesign -d --entitlements :- "$A" 2>/dev/null || true
}
adhoc_sign || echo "warning: ad-hoc signing failed; the IPA carries no entitlements, so the sideloader must add increased-memory-limit itself"
rm -f "$IPA"
(cd "$STAGE" && zip -qry -X "$IPA" Payload)
rm -rf "$STAGE"
echo "IPA: $IPA ($(wc -c < "$IPA" | tr -d ' ') bytes)"
