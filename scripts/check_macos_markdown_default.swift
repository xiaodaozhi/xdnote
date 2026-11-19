import AppKit
import ApplicationServices
import CoreGraphics
import CoreServices
import Foundation
import UniformTypeIdentifiers

private struct CheckError: LocalizedError {
    let errorDescription: String?
    init(_ message: String) { errorDescription = message }
}

private final class Setting: @unchecked Sendable {
    private let lock = NSLock()
    private var value: String?
    func complete(_ error: Error?) {
        lock.lock()
        value = error?.localizedDescription ?? ""
        lock.unlock()
    }
    func message() -> String? {
        lock.lock()
        defer { lock.unlock() }
        return value
    }
}

private func require(_ value: Bool, _ message: String) throws {
    if !value { throw CheckError(message) }
}

private func waitFor<T>(_ description: String, _ probe: () -> T?) throws -> T {
    let deadline = Date().addingTimeInterval(20)
    while Date() < deadline {
        if let value = probe() { return value }
        RunLoop.current.run(until: Date().addingTimeInterval(0.1))
    }
    throw CheckError("Timed out waiting for " + description)
}

private func axValue(_ item: AXUIElement, _ attribute: String) -> CFTypeRef? {
    var value: CFTypeRef?
    return AXUIElementCopyAttributeValue(item, attribute as CFString, &value) == .success ? value : nil
}

private func markdownConfirmationButton(for application: URL) -> AXUIElement? {
    guard AXIsProcessTrusted(), let bundle = Bundle(url: application) else { return nil }
    let names = ["CFBundleDisplayName", "CFBundleName"].compactMap {
        bundle.object(forInfoDictionaryKey: $0) as? String
    } + [application.deletingPathExtension().lastPathComponent]
    let captions = Set(names.map { "Use “" + $0 + "”" })
    let options: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
    let windows = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[String: Any]] ?? []
    let pids = Set(windows.compactMap { window -> Int32? in
        guard window[kCGWindowOwnerName as String] as? String == "CoreServicesUIAgent" else { return nil }
        return window[kCGWindowOwnerPID as String] as? Int32
    })
    var choices: [AXUIElement] = []
    for pid in pids {
        let agent = AXUIElementCreateApplication(pid)
        let alerts = (axValue(agent, "AXWindows") as? [AXUIElement]) ?? []
        for alert in alerts {
            let children = (axValue(alert, "AXChildren") as? [AXUIElement]) ?? []
            let messageMatches = children.contains { node in
                guard (axValue(node, "AXRole") as? String) == "AXStaticText",
                      let message = axValue(node, "AXValue") as? String else { return false }
                return message.contains("all documents with the extensions “.md” and “.markdown”")
                    && names.contains(where: { message.contains("to open with “" + $0 + "”") })
            }
            if messageMatches {
                choices += children.filter {
                    (axValue($0, "AXRole") as? String) == "AXButton"
                        && captions.contains(axValue($0, "AXTitle") as? String ?? "")
                }
            }
        }
    }
    return choices.count == 1 ? choices.first : nil
}

@discardableResult private func setTypeDefault(_ application: URL, for file: URL) throws -> Bool {
    let result = Setting()
    NSWorkspace.shared.setDefaultApplication(at: application, toOpenContentTypeOfFileAt: file) { error in
        result.complete(error)
    }
    var confirmed = false
    let message = try waitFor("the NSWorkspace file-type setting") {
        if let message = result.message() { return message }
        if !confirmed, let button = markdownConfirmationButton(for: application) {
            confirmed = AXUIElementPerformAction(button, kAXPressAction as CFString) == .success
        }
        return nil
    }
    try require(message.isEmpty, "NSWorkspace file-type setting failed: " + message)
    return confirmed
}

private func defaultApplication(_ file: URL) -> URL? {
    NSWorkspace.shared.urlForApplication(toOpen: file)?.resolvingSymlinksInPath()
}

private func windowNumbers(_ pid: pid_t) -> Set<Int> {
    let options: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
    let windows = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[String: Any]] ?? []
    return Set(windows.compactMap { window in
        guard window[kCGWindowOwnerPID as String] as? Int32 == pid,
              window[kCGWindowLayer as String] as? Int == 0 else { return nil }
        return window[kCGWindowNumber as String] as? Int
    })
}

private func write(_ report: [String: Any], to url: URL) throws {
    try JSONSerialization.data(withJSONObject: report, options: [.prettyPrinted, .sortedKeys]).write(to: url)
}

let args = CommandLine.arguments
guard args.count == 3, getuid() != 0,
      ProcessInfo.processInfo.environment["GITHUB_ACTIONS"] == "true",
      ProcessInfo.processInfo.environment["RUNNER_ENVIRONMENT"] == "github-hosted" else {
    FileHandle.standardError.write(Data("Use the disposable hosted macOS account.\n".utf8))
    exit(2)
}
let app = URL(fileURLWithPath: args[1], isDirectory: true).resolvingSymlinksInPath()
let evidence = URL(fileURLWithPath: args[2], isDirectory: true)
let result = evidence.appendingPathComponent("result.json")
let markdown = "net.daringfireball.markdown"
let workspace = NSWorkspace.shared
let role = LSRolesMask.all
let id = Bundle(url: app)?.bundleIdentifier ?? ""
var report: [String: Any] = ["ok": false, "contentType": markdown, "bundleIdentifier": id]
var original: String?
var originalApplication: URL?
var referenceFile: URL?
var changed = false
var failure = false

do {
    try require(!id.isEmpty && app.pathExtension == "app", "Invalid sealed app bundle")
    try require(NSRunningApplication.runningApplications(withBundleIdentifier: id).isEmpty, "An xdnote instance is already running")
    let md = evidence.appendingPathComponent("冷启动 Markdown.md")
    let markdownLong = evidence.appendingPathComponent("运行中 转交.markdown")
    let txt = evidence.appendingPathComponent("无关类型.txt")
    let inputs = [md, markdownLong, txt]
    let source = Data("# Owned Markdown acceptance\n".utf8)
    for input in inputs { try source.write(to: input, options: [.withoutOverwriting]) }

    original = LSCopyDefaultRoleHandlerForContentType(markdown as CFString, role)?.takeRetainedValue() as String?
    originalApplication = defaultApplication(md)
    referenceFile = md
    let unrelatedApp = defaultApplication(txt)
    try require(LSRegisterURL(app as CFURL, true) == noErr, "Launch Services could not register xdnote")
    for file in [md, markdownLong] {
        let type = try file.resourceValues(forKeys: [.contentTypeKey]).contentType?.identifier
        try require(type == markdown, "Unexpected UTI for " + file.pathExtension + ": " + (type ?? "(none)"))
    }
    let baseline = [md, markdownLong].map { defaultApplication($0)?.path ?? "(none)" }
    report["previousHandler"] = original ?? "(none)"
    report["resolvedBefore"] = baseline
    changed = true
    report["confirmedSystemDialog"] = try setTypeDefault(app, for: md)
    _ = try waitFor("the Markdown type handler and both file extensions") {
        let current = LSCopyDefaultRoleHandlerForContentType(markdown as CFString, role)?.takeRetainedValue() as String?
        let resolved = [md, markdownLong].map { defaultApplication($0)?.path ?? "(none)" }
        report["observedHandler"] = current ?? "(none)"
        report["resolvedAfter"] = resolved
        return current == id && resolved == [app.path, app.path] ? true : nil
    }
    try require(defaultApplication(txt) == unrelatedApp, "The unrelated text handler changed")
    let later = evidence.appendingPathComponent("设默认后新建.md")
    try source.write(to: later, options: [.withoutOverwriting])
    try require(defaultApplication(later) == app, "A new Markdown file did not inherit the type handler")
    report["selectedDefault"] = id
    report["resolvedExtensions"] = ["md", "markdown"]
    report["newFileInheritedDefault"] = true
    report["unrelatedTextDefaultUnchanged"] = true

    try require(workspace.open(md), "The default open request was rejected")
    let launched = try waitFor("the exact installed bundle") {
        NSRunningApplication.runningApplications(withBundleIdentifier: id).first {
            $0.bundleURL?.resolvingSymlinksInPath() == app
        }
    }
    let previousWindows = try waitFor("the first native application window") {
        let numbers = windowNumbers(launched.processIdentifier)
        return numbers.isEmpty ? nil : numbers
    }
    try require(workspace.open(markdownLong), "The second default open request was rejected")
    let secondWindows = try waitFor("another native window in the same process") {
        let numbers = windowNumbers(launched.processIdentifier)
        return numbers.subtracting(previousWindows).isEmpty ? nil : numbers
    }
    let pids = NSRunningApplication.runningApplications(withBundleIdentifier: id).map(\.processIdentifier)
    try require(pids == [launched.processIdentifier], "The second document did not reuse the installed application")
    for file in inputs + [later] {
        let bytes = try Data(contentsOf: file)
        try require(bytes == source, "A Markdown acceptance sample changed")
    }
    report["openedSamples"] = [md.lastPathComponent, markdownLong.lastPathComponent]
    report["windowsBeforeWarmOpen"] = previousWindows.count
    report["windowsAfterWarmOpen"] = secondWindows.count
    report["sameProcess"] = true
    report["fixtureBytesUnchanged"] = true
    report["ok"] = true
} catch {
    report["error"] = error.localizedDescription
    failure = true
}

for process in NSRunningApplication.runningApplications(withBundleIdentifier: id)
    where process.bundleURL?.resolvingSymlinksInPath() == app {
    _ = process.terminate()
}
if changed, let previous = originalApplication, let file = referenceFile, previous != app {
    do {
        report["confirmedRestoreDialog"] = try setTypeDefault(previous, for: file)
        _ = try waitFor("the restored Markdown default") {
            defaultApplication(file) == previous ? true : nil
        }
        report["restoredPreviousHandler"] = true
    } catch {
        report["restoredPreviousHandler"] = false
        report["cleanupError"] = error.localizedDescription
        failure = true
    }
}
if failure { report["ok"] = false }
try write(report, to: result)
if failure { exit(1) }
