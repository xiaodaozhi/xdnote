import ApplicationServices
import Foundation

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(1)
}

func attribute(_ element: AXUIElement, _ name: String) -> CFTypeRef? {
    var value: CFTypeRef?
    return AXUIElementCopyAttributeValue(element, name as CFString, &value) == .success ? value : nil
}

func editor(in element: AXUIElement, depth: Int = 0) -> AXUIElement? {
    if attribute(element, kAXValueAttribute) as? String == "old document" { return element }
    guard depth < 8 else { return nil }
    for child in attribute(element, kAXChildrenAttribute) as? [AXUIElement] ?? [] {
        if let match = editor(in: child, depth: depth + 1) { return match }
    }
    return nil
}

func send(_ line: String) {
    FileHandle.standardOutput.write(Data((line + "\n").utf8))
}

guard CommandLine.arguments.count == 2, let pid = Int32(CommandLine.arguments[1]), pid > 1 else {
    fail("Expected the owned test process ID")
}
guard AXIsProcessTrusted() else { fail("The AX client needs Accessibility permission") }
let app = AXUIElementCreateApplication(pid)
_ = AXUIElementSetMessagingTimeout(app, 2)
var found: AXUIElement?
for _ in 0..<30 {
    let windows = attribute(app, kAXWindowsAttribute) as? [AXUIElement] ?? []
    if let window = windows.first(where: { attribute($0, kAXTitleAttribute) as? String == "xdnote AX research fixture" }),
       let text = editor(in: window) {
        found = text
        break
    }
    Thread.sleep(forTimeInterval: 0.1)
}
guard let text = found else { fail("Could not find the editor in the owned Cocoa fixture") }
send("READY")
guard readLine() == "CHECK" else { fail("Missing replacement checkpoint") }
let replacement = attribute(text, kAXValueAttribute) as? String
let selectedText = attribute(text, kAXSelectedTextAttribute) as? String
guard replacement == "new source" else { fail("The cached AX object did not expose the replacement value") }
guard selectedText == "source" else { fail("The cached AX object did not expose the replacement selection") }
var firstWord = CFRange(location: 0, length: 3)
guard let range = AXValueCreate(.cfRange, &firstWord) else { fail("Could not encode selection range") }
let selection = AXUIElementSetAttributeValue(text, kAXSelectedTextRangeAttribute as CFString, range)
guard selection == .success, attribute(text, kAXSelectedTextAttribute) as? String == "new" else {
    fail("AX selection write did not take effect (\(selection.rawValue))")
}
send("SELECTION_CHECKED")
guard readLine() == "READONLY" else { fail("Missing read-only checkpoint") }
_ = AXUIElementSetAttributeValue(text, kAXValueAttribute as CFString, "rejected" as CFString)
guard attribute(text, kAXValueAttribute) as? String == "new source" else { fail("Read-only AX write changed text") }
send("READONLY_CHECKED")
guard readLine() == "WRITE" else { fail("Missing writable checkpoint") }
let write = AXUIElementSetAttributeValue(text, kAXValueAttribute as CFString, "written through AX" as CFString)
guard write == .success, attribute(text, kAXValueAttribute) as? String == "written through AX" else {
    fail("Writable AX value did not take effect (\(write.rawValue))")
}
