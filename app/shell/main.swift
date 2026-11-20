// SPDX-License-Identifier: GPL-3.0-only
// Chordially: the Mac app. The window shows the interface (index.html in a web view); the sound, the sequencer and
// MIDI run natively in C (shell.c, ../core). The page sends JSON messages ("cm"); the state and the live view come
// back 60 times a second through __cmIn([...]). Built by "Build Chordially.command" (no Xcode project).
import Cocoa
import WebKit

final class PageBridge: NSObject, WKScriptMessageHandler {
    func userContentController(_ ucc: WKUserContentController, didReceive message: WKScriptMessage) {
        guard let s = message.body as? String else { return }
        if message.name != "cm" { sh_page_log(s); return }
        if s.hasPrefix("{\"t\":\"plug") { insertHost.handle(s) } else { sh_msg(s) }   // (the AU insert: insert.swift)
    }
}

enum PageScripts {
    static let console = """
    (function () {
      const L = m => { try { window.webkit.messageHandlers.log.postMessage(String(m)); } catch (e) { } };
      window.addEventListener("error", e => L("error: " + e.message + " at " + e.filename + ":" + e.lineno));
      window.addEventListener("unhandledrejection", e => L("rejection: " + (e.reason && e.reason.stack || e.reason)));
      const er = console.error.bind(console);
      console.error = (...a) => { L("error: " + a.join(" ")); er(...a); };
    })();
    """
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    var window: NSWindow!
    var web: WKWebView!
    let bridge = PageBridge()
    var timer: Timer?
    var ticks = 0

    func applicationDidFinishLaunching(_ notification: Notification) {
        buildMenu()
        let ucc = WKUserContentController()
        ucc.add(bridge, name: "cm")
        ucc.add(bridge, name: "log")
        ucc.addUserScript(WKUserScript(source: PageScripts.console, injectionTime: .atDocumentStart, forMainFrameOnly: true))
        let cfg = WKWebViewConfiguration()
        cfg.userContentController = ucc
        cfg.preferences.setValue(true, forKey: "developerExtrasEnabled")
        let vis = NSScreen.main?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1512, height: 945)
        let w = min(vis.width, 1512), h = min(vis.height, 980)
        let frame = NSRect(x: vis.midX - w / 2, y: vis.midY - h / 2, width: w, height: h)
        web = WKWebView(frame: NSRect(origin: .zero, size: frame.size), configuration: cfg)
        if #available(macOS 13.3, *) { web.isInspectable = true }
        window = NSWindow(contentRect: frame, styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        window.title = "Chordially"
        window.contentView = web
        window.minSize = NSSize(width: 1200, height: 800)
        window.setFrameAutosaveName("Chordially main")
        window.collectionBehavior.insert(.fullScreenPrimary)
        window.makeKeyAndOrderFront(nil)
        window.makeFirstResponder(web)
        NSApp.activate(ignoringOtherApps: true)

        if sh_audio_start() != 0 { sh_note("the audio output did not start") }
        if sh_midi_start() != 0 { sh_note("MIDI did not start") }

        insertHost.start(page: web)
        guard let page = Bundle.main.url(forResource: "index", withExtension: "html") else { sh_note("index.html missing"); return }
        web.loadFileURL(page, allowingReadAccessTo: page.deletingLastPathComponent())
        timer = Timer.scheduledTimer(timeInterval: 1.0 / 60.0, target: self, selector: #selector(pump), userInfo: nil, repeats: true)
        RunLoop.main.add(timer!, forMode: .common)
    }

    @objc func pump() {
        if let p = sh_drain() {
            let s = String(cString: p)
            free(p)
            web.evaluateJavaScript("window.__cmIn && __cmIn(" + s + ")", completionHandler: nil)
        }
        ticks += 1
        if ticks % 60 == 0 { sh_tick() }
        if ticks % (60 * 30) == 0 { insertHost.save(); insertHost.saveInstrument() }   // (the plugin's own settings, every 30 s and at quit)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { return true }
    func applicationWillTerminate(_ notification: Notification) {
        // In order: the page stops, the plugins' settings are saved, the audio and USB inputs stop (no more render
        // calls into the plugins), the plugin engines stop, then exit at once. A normal exit would run the plugins'
        // own C++ teardown while their threads still run ("mutex lock failed", the crash report on quit).
        timer?.invalidate()
        insertHost.quit()
        sh_quit()
        insertHost.stopEngines()
        sh_note("quit: in order")
        _exit(0)
    }

    func buildMenu() {
        let main = NSMenu()
        let appItem = NSMenuItem(); main.addItem(appItem)
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "Quit Chordially", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu
        let editItem = NSMenuItem(); main.addItem(editItem)
        let edit = NSMenu(title: "Edit")
        edit.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        edit.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        edit.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        editItem.submenu = edit
        let viewItem = NSMenuItem(); main.addItem(viewItem)
        let view = NSMenu(title: "View")
        let fs = NSMenuItem(title: "Enter / Exit Full Screen", action: #selector(NSWindow.toggleFullScreen(_:)), keyEquivalent: "f")
        fs.keyEquivalentModifierMask = [.command, .control]
        view.addItem(fs)
        viewItem.submenu = view
        NSApp.mainMenu = main
    }
}

// Where Chordially keeps its data (chordially.json, insert.json, instrument.json, logs/):
//  - a folder "Chordially data" next to Chordially.app, if there is one (portable: the app and its data together),
//  - otherwise ~/Library/Application Support/Chordially (the normal place for an installed app).
let dataDir: String = {
    if let p = ProcessInfo.processInfo.environment["CHORDIALLY_DATA"] { return p }
    let beside = (Bundle.main.bundlePath as NSString).deletingLastPathComponent + "/Chordially data"
    var isDir: ObjCBool = false
    if FileManager.default.fileExists(atPath: beside, isDirectory: &isDir), isDir.boolValue { return beside }
    return NSHomeDirectory() + "/Library/Application Support/Chordially"
}()
try? FileManager.default.createDirectory(atPath: dataDir + "/logs", withIntermediateDirectories: true)
func logPath() -> String {
    if let p = ProcessInfo.processInfo.environment["CM_LOG"] { return p }
    let fm = FileManager.default, dir = dataDir + "/logs"
    let old = ((try? fm.contentsOfDirectory(atPath: dir)) ?? []).filter { $0.hasSuffix(".log") }.sorted()
    if old.count >= 10 { for f in old.prefix(old.count - 9) { try? fm.removeItem(atPath: dir + "/" + f) } }
    let df = DateFormatter(); df.dateFormat = "yyyy-MM-dd_HH-mm-ss"
    return dir + "/chordially-" + df.string(from: Date()) + ".log"
}
_ = sh_init(logPath(), dataDir)
let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
