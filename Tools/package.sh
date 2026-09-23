#!/usr/bin/env bash
# Builds dist/NoteCap-<version>.pkg: an installer with AU and VST3 choices that
# installs to /Library/Audio/Plug-Ins.
#
#   Tools/package.sh              # build Release, then package
#   Tools/package.sh --no-build   # package the existing Release build
#
# Signing and notarization switch on when these are set (otherwise the plugins
# are ad-hoc signed: fine on this Mac, blocked by Gatekeeper elsewhere):
#   DEVELOPER_ID_APP="Developer ID Application: Your Name (TEAMID)"
#   DEVELOPER_ID_INSTALLER="Developer ID Installer: Your Name (TEAMID)"
#   NOTARY_PROFILE=notecap   # from: xcrun notarytool store-credentials notecap ...
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(cd "$HERE/.." && pwd)"
REL="$PROJ/Builds/MacOSX/build/Release"
OUT="$PROJ/dist"
VERSION="$(python3 -c "import sys, xml.etree.ElementTree as ET; print(ET.parse(sys.argv[1]).getroot().get('version'))" "$PROJ/NoteCap.jucer")"
ID_BASE="com.alistairrutherford.notecap"

if [[ "${1:-}" != "--no-build" ]]; then
    echo "== building Release =="
    xcodebuild -project "$PROJ/Builds/MacOSX/NoteCap.xcodeproj" -target "NoteCap - All" \
               -configuration Release build | grep -E "error:|BUILD (SUCCEEDED|FAILED)" | sort -u
fi

for b in NoteCap.component NoteCap.vst3; do
    [[ -d "$REL/$b" ]] || { echo "missing $REL/$b (build Release first)" >&2; exit 2; }
done

rm -rf "$OUT/stage" "$OUT/pkgs"
mkdir -p "$OUT/stage/au" "$OUT/stage/vst3" "$OUT/pkgs" "$OUT/scripts"
ditto "$REL/NoteCap.component" "$OUT/stage/au/NoteCap.component"
ditto "$REL/NoteCap.vst3" "$OUT/stage/vst3/NoteCap.vst3"

# ---- sign the plugin bundles ----
for b in "$OUT/stage/au/NoteCap.component" "$OUT/stage/vst3/NoteCap.vst3"; do
    if [[ -n "${DEVELOPER_ID_APP:-}" ]]; then
        codesign --force --options runtime --timestamp --sign "$DEVELOPER_ID_APP" "$b"
    else
        codesign --force --sign - "$b"
    fi
    codesign --verify --strict "$b"
done
SIGNED_WITH="${DEVELOPER_ID_APP:-ad-hoc}"

# Refresh the AU cache so Live sees the new version without a reboot.
cat > "$OUT/scripts/postinstall" <<'EOF'
#!/bin/sh
killall -9 AudioComponentRegistrar 2>/dev/null || true
exit 0
EOF
chmod +x "$OUT/scripts/postinstall"

# ---- component packages ----
# Bundles must not be "relocatable": otherwise Installer puts the update wherever
# it finds an existing bundle with the same ID (e.g. a dev build in ~/Library).
component_pkg () {
    local kind="$1" dest="$2"
    local plist="$OUT/pkgs/$kind.plist"
    pkgbuild --analyze --root "$OUT/stage/$kind" "$plist" >/dev/null
    local n
    n=$(plutil -convert json -o - "$plist" | python3 -c "import json,sys; print(len(json.load(sys.stdin)))")
    for ((i = 0; i < n; i++)); do
        plutil -replace "$i.BundleIsRelocatable" -bool NO "$plist"
    done
    pkgbuild --root "$OUT/stage/$kind" --component-plist "$plist" --install-location "$dest" \
             --identifier "$ID_BASE.$kind" --version "$VERSION" --scripts "$OUT/scripts" \
             "$OUT/pkgs/NoteCap-$kind.pkg" >/dev/null
}
component_pkg au   "/Library/Audio/Plug-Ins/Components"
component_pkg vst3 "/Library/Audio/Plug-Ins/VST3"

cat > "$OUT/pkgs/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>NoteCap $VERSION</title>
    <options customize="allow" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_localSystem="true"/>
    <volume-check><allowed-os-versions><os-version min="12.0"/></allowed-os-versions></volume-check>
    <choices-outline>
        <line choice="au"/>
        <line choice="vst3"/>
    </choices-outline>
    <choice id="au" title="Audio Unit (Ableton Live, Logic)" description="Installs NoteCap.component to /Library/Audio/Plug-Ins/Components.">
        <pkg-ref id="$ID_BASE.au"/>
    </choice>
    <choice id="vst3" title="VST3" description="Installs NoteCap.vst3 to /Library/Audio/Plug-Ins/VST3.">
        <pkg-ref id="$ID_BASE.vst3"/>
    </choice>
    <pkg-ref id="$ID_BASE.au" version="$VERSION">NoteCap-au.pkg</pkg-ref>
    <pkg-ref id="$ID_BASE.vst3" version="$VERSION">NoteCap-vst3.pkg</pkg-ref>
</installer-gui-script>
EOF

PKG="$OUT/NoteCap-$VERSION.pkg"
rm -f "$PKG"
if [[ -n "${DEVELOPER_ID_INSTALLER:-}" ]]; then
    productbuild --distribution "$OUT/pkgs/distribution.xml" --package-path "$OUT/pkgs" \
                 --sign "$DEVELOPER_ID_INSTALLER" --timestamp "$PKG" >/dev/null
else
    productbuild --distribution "$OUT/pkgs/distribution.xml" --package-path "$OUT/pkgs" "$PKG" >/dev/null
fi

# ---- notarize ----
if [[ -n "${NOTARY_PROFILE:-}" ]]; then
    if [[ -z "${DEVELOPER_ID_APP:-}" || -z "${DEVELOPER_ID_INSTALLER:-}" ]]; then
        echo "NOTARY_PROFILE set but Developer ID identities missing; skipping notarization" >&2
    else
        xcrun notarytool submit "$PKG" --keychain-profile "$NOTARY_PROFILE" --wait
        xcrun stapler staple "$PKG"
    fi
fi

echo ""
echo "built $PKG"
echo "  version $VERSION, plugins signed: $SIGNED_WITH, installer signed: ${DEVELOPER_ID_INSTALLER:-no}"
echo "  notarized: $([[ -n "${NOTARY_PROFILE:-}" && -n "${DEVELOPER_ID_INSTALLER:-}" ]] && echo yes || echo no)"
