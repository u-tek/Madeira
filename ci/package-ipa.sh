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
rm -f "$IPA"
(cd "$STAGE" && zip -qry -X "$IPA" Payload)
rm -rf "$STAGE"
echo "IPA: $IPA ($(wc -c < "$IPA" | tr -d ' ') bytes)"
