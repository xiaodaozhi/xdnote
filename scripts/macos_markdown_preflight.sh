#!/usr/bin/env bash
set -euo pipefail
out="$PWD/build/preflight"
app="$out/Markdown Fixture.app"
mkdir -p "$app/Contents/MacOS" "$out/legacy" "$out/workspace"
cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "https://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>io.github.xiaodaozhi.xdnote.markdown.fixture</string>
<key>CFBundleExecutable</key><string>fixture</string>
<key>CFBundleName</key><string>Markdown Fixture</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>1</string>
<key>CFBundleDocumentTypes</key><array><dict>
<key>CFBundleTypeName</key><string>Markdown</string>
<key>CFBundleTypeRole</key><string>Editor</string>
<key>LSHandlerRank</key><string>Alternate</string>
<key>LSItemContentTypes</key><array><string>net.daringfireball.markdown</string></array>
</dict></array>
<key>UTImportedTypeDeclarations</key><array><dict>
<key>UTTypeIdentifier</key><string>net.daringfireball.markdown</string>
<key>UTTypeConformsTo</key><array><string>public.plain-text</string></array>
<key>UTTypeTagSpecification</key><dict>
<key>public.filename-extension</key><array><string>md</string><string>markdown</string></array>
<key>public.mime-type</key><string>text/markdown</string>
</dict></dict></array>
</dict></plist>
PLIST
cat > "$out/editor.swift" <<'SWIFT'
import AppKit
final class Editor: NSObject, NSApplicationDelegate {
    var windows: [NSWindow] = []
    func application(_ application: NSApplication, open urls: [URL]) {
        for url in urls {
            let window = NSWindow(contentRect: NSRect(x: 80, y: 80, width: 360, height: 230),
                                  styleMask: [.titled, .closable], backing: .buffered, defer: false)
            window.title = url.lastPathComponent
            window.makeKeyAndOrderFront(nil)
            windows.append(window)
        }
        application.activate()
    }
}
let application = NSApplication.shared
let delegate = Editor()
application.delegate = delegate
application.setActivationPolicy(.regular)
application.run()
SWIFT
xcrun swiftc "$out/editor.swift" -module-cache-path "$out/cache" -o "$app/Contents/MacOS/fixture"
/usr/bin/codesign --force --sign - "$app"
xcrun swiftc scripts/check_macos_markdown_default.swift -module-cache-path "$out/cache" -o "$out/probe"
for kind in legacy workspace; do
    XDNOTE_DEFAULT_API="$kind" "$out/probe" "$app" "$out/$kind" || true
    cat "$out/$kind/result.json"
    sleep 3
done
