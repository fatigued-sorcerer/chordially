// SPDX-License-Identifier: GPL-3.0-only
// Chordially: ONE Audio Unit insert effect on the chord sound (simplified from Jammer's plugins.swift).
// An AVAudioEngine of its own in manual rendering mode (48 kHz stereo): source (the chord sound) -> effect -> out.
// The core calls insertRenderC on the audio thread for every 128-frame block, after the synth and before volume and
// the delay / reverb sends; it processes in place. Tempo and transport come from Chordially's clock. The plugin and
// its whole sound are kept in "Chordially data/insert.json" and loaded again at start.
import Cocoa
import AVFoundation
import AudioToolbox
import CoreAudioKit
import WebKit

final class InsertChain: @unchecked Sendable {
    let engine = AVAudioEngine()
    var block: AVAudioEngineManualRenderingBlock?
    var buffer: AVAudioPCMBuffer?
    var ok = false
    var fx: AVAudioUnit?
    var comp: AVAudioUnitComponent?
    var bypass = false
    var source: AVAudioSourceNode?
    var inL: UnsafeMutablePointer<Float>?
    var inR: UnsafeMutablePointer<Float>?
    let lock = NSLock()
    var renders = 0, skipped = 0
    func locked(_ body: () -> Void) { lock.lock(); body(); lock.unlock() }   // (callable from a Task)

    // audio thread
    func render(_ L: UnsafeMutablePointer<Float>, _ R: UnsafeMutablePointer<Float>, _ n: Int) -> Bool {
        guard lock.try() else { skipped += 1; return false }
        defer { lock.unlock() }
        guard ok, fx != nil, !bypass, let block = block, let buf = buffer, let data = buf.floatChannelData else { return false }
        inL = L; inR = R
        buf.frameLength = AVAudioFrameCount(n)
        var err: OSStatus = 0
        let st = block(AVAudioFrameCount(n), buf.mutableAudioBufferList, &err)
        inL = nil; inR = nil
        if st != .success { return false }
        let l = data[0], r = buf.format.channelCount > 1 ? data[1] : data[0]
        for i in 0..<n { L[i] = l[i]; R[i] = r[i] }
        renders += 1
        return true
    }
}
let insertChain = InsertChain()
func insertRenderC(_ L: UnsafeMutablePointer<Float>?, _ R: UnsafeMutablePointer<Float>?, _ n: Int32) -> Int32 {
    guard let L = L, let R = R else { return 0 }
    return insertChain.render(L, R, Int(n)) ? 1 : 0
}

// ---- the AU instrument (SOUND > INSTRUMENT): the chord part's notes, sample accurate, rendered instead of AURA
final class InstrChain: @unchecked Sendable {
    let engine = AVAudioEngine()
    var block: AVAudioEngineManualRenderingBlock?
    var buffer: AVAudioPCMBuffer?
    var ok = false
    var inst: AVAudioUnit?
    var comp: AVAudioUnitComponent?
    var sched: AUScheduleMIDIEventBlock?
    let lock = NSLock()
    func locked(_ body: () -> Void) { lock.lock(); body(); lock.unlock() }
    func render(_ ev: UnsafePointer<Int32>?, _ nev: Int, _ L: UnsafeMutablePointer<Float>, _ R: UnsafeMutablePointer<Float>, _ n: Int) -> Bool {
        guard lock.try() else { return false }
        defer { lock.unlock() }
        guard ok, inst != nil, let block = block, let buf = buffer, let data = buf.floatChannelData else { return false }
        if let s = sched, let ev = ev {
            for k in 0..<nev {
                var b: (UInt8, UInt8, UInt8) = (UInt8(ev[4 * k + 1] & 0xFF), UInt8(ev[4 * k + 2] & 127), UInt8(ev[4 * k + 3] & 127))
                withUnsafeBytes(of: &b) { p in
                    s(AUEventSampleTimeImmediate + AUEventSampleTime(ev[4 * k]), 0, 3, p.bindMemory(to: UInt8.self).baseAddress!)
                }
            }
        }
        buf.frameLength = AVAudioFrameCount(n)
        var err: OSStatus = 0
        if block(AVAudioFrameCount(n), buf.mutableAudioBufferList, &err) != .success { return false }
        let l = data[0], r = buf.format.channelCount > 1 ? data[1] : data[0]
        for i in 0..<n { L[i] = l[i]; R[i] = r[i] }
        return true
    }
}
let instrChain = InstrChain()
func instrRenderC(_ ev: UnsafePointer<Int32>?, _ nev: Int32, _ L: UnsafeMutablePointer<Float>?, _ R: UnsafeMutablePointer<Float>?, _ n: Int32) -> Int32 {
    guard let L = L, let R = R else { return 0 }
    return instrChain.render(ev, Int(nev), L, R, Int(n)) ? 1 : 0
}

// Chordially's keys while the plugin's own window is in front: they play Chordially and stop there (no beep).
enum KeyMap {
    static let codes: [UInt16: String] = [
        0: "KeyA", 1: "KeyS", 2: "KeyD", 3: "KeyF", 4: "KeyH", 5: "KeyG", 6: "KeyZ", 7: "KeyX", 13: "KeyW", 14: "KeyE",
        16: "KeyY", 17: "KeyT", 32: "KeyU", 38: "KeyJ", 40: "KeyK", 49: "Space", 53: "Escape"]
}
final class PlugWindow: NSWindow {
    var page: WKWebView?
    override func sendEvent(_ ev: NSEvent) {
        if ev.type == .keyDown || ev.type == .keyUp {
            let typing = firstResponder is NSText
            let mods = !ev.modifierFlags.intersection([.command, .control, .option]).isEmpty
            if !typing && !mods, let code = KeyMap.codes[ev.keyCode] {
                let down = ev.type == .keyDown, rep = down && ev.isARepeat
                page?.evaluateJavaScript("window.__cmKey && __cmKey('\(code)', \(down), \(rep))", completionHandler: nil)
                return
            }
        }
        super.sendEvent(ev)
    }
}

final class InsertHost: NSObject, @unchecked Sendable {
    var effects: [AVAudioUnitComponent] = []
    var instruments: [AVAudioUnitComponent] = []
    var instWindow: NSWindow?
    var instLoading = false
    let instFile = dataDir + "/instrument.json"
    var format: AVAudioFormat!
    var window: NSWindow?
    weak var page: WKWebView?
    var loading = false
    let file = dataDir + "/insert.json"

    func note(_ s: String) { sh_note("insert: " + s) }
    func toPage(_ obj: [String: Any]) {
        if let d = try? JSONSerialization.data(withJSONObject: obj), let s = String(data: d, encoding: .utf8) { sh_post(s) }
    }
    func title(_ c: AVAudioUnitComponent) -> String { return c.manufacturerName.isEmpty ? c.name : c.manufacturerName + " – " + c.name }
    func ident(_ c: AVAudioUnitComponent) -> String {
        let d = c.audioComponentDescription
        return "\(d.componentType):\(d.componentSubType):\(d.componentManufacturer)"
    }

    func start(page: WKWebView) {
        self.page = page
        guard let f = AVAudioFormat(standardFormatWithSampleRate: 48000, channels: 2) else { note("FAILED: no format"); return }
        format = f
        let c = insertChain
        do {
            try c.engine.enableManualRenderingMode(.realtime, format: f, maximumFrameCount: 4096)
            let src = AVAudioSourceNode(format: f) { _, _, frames, abl -> OSStatus in
                let list = UnsafeMutableAudioBufferListPointer(abl)
                let n = Int(frames)
                for (k, b) in list.enumerated() {
                    guard let d = b.mData?.assumingMemoryBound(to: Float.self) else { continue }
                    if let s = (k == 0 ? c.inL : c.inR) { for i in 0..<n { d[i] = s[i] } } else { for i in 0..<n { d[i] = 0 } }
                }
                return noErr
            }
            c.engine.attach(src)
            c.source = src
            c.engine.connect(src, to: c.engine.mainMixerNode, format: f)
            try c.engine.start()
            c.block = c.engine.manualRenderingBlock
            c.buffer = AVAudioPCMBuffer(pcmFormat: c.engine.manualRenderingFormat, frameCapacity: 4096)
            c.ok = c.buffer != nil
        } catch { note("its engine FAILED: \(error)") }
        sh_set_insert(insertRenderC)
        let ic = instrChain
        do {
            try ic.engine.enableManualRenderingMode(.realtime, format: f, maximumFrameCount: 4096)
            _ = ic.engine.mainMixerNode
            try ic.engine.start()
            ic.block = ic.engine.manualRenderingBlock
            ic.buffer = AVAudioPCMBuffer(pcmFormat: ic.engine.manualRenderingFormat, frameCapacity: 4096)
            ic.ok = ic.buffer != nil
        } catch { note("instrument engine FAILED: \(error)") }
        sh_set_instr(instrRenderC)
        scan()
        restore()
        restoreInstrument()
    }
    func scan() {
        var e = AudioComponentDescription(); e.componentType = kAudioUnitType_Effect
        var m = AudioComponentDescription(); m.componentType = kAudioUnitType_MusicEffect
        effects = (AVAudioUnitComponentManager.shared().components(matching: e) + AVAudioUnitComponentManager.shared().components(matching: m))
            .sorted { title($0).lowercased() < title($1).lowercased() }
        var w = AudioComponentDescription(); w.componentType = kAudioUnitType_MusicDevice
        instruments = AVAudioUnitComponentManager.shared().components(matching: w).sorted { title($0).lowercased() < title($1).lowercased() }
        note("\(effects.count) effects, \(instruments.count) instruments installed")
    }
    func sendList() { toPage(["t": "plugs", "fx": effects.map { ["id": ident($0), "name": title($0)] }, "inst": instruments.map { ["id": ident($0), "name": title($0)] }]) }
    func sendInst() {
        let c = instrChain
        toPage(["t": "instrument", "id": c.comp.map { ident($0) } ?? "", "name": c.comp.map { title($0) } ?? "", "loading": instLoading])
    }
    func sendState() {
        let c = insertChain
        toPage(["t": "insert", "id": c.comp.map { ident($0) } ?? "", "name": c.comp.map { title($0) } ?? "", "byp": c.bypass, "loading": loading])
    }

    // page: plughello, plugfx {id}, plugbyp {on}, plugwin
    func handle(_ json: String) {
        guard let d = json.data(using: .utf8), let m = (try? JSONSerialization.jsonObject(with: d)) as? [String: Any], let t = m["t"] as? String else { return }
        switch t {
        case "plughello": sendList(); sendState(); sendInst()
        case "pluginst":
            let id = m["id"] as? String ?? ""
            if id.isEmpty { setInstrument(nil, state: nil) }
            else if let c = instruments.first(where: { ident($0) == id }) { setInstrument(c, state: nil) }
        case "pluginstwin": openInstWindow()
        case "plugfx":
            let id = m["id"] as? String ?? ""
            if id.isEmpty { set(nil, state: nil) }
            else if let c = effects.first(where: { ident($0) == id }) { set(c, state: nil) }
        case "plugbyp":
            insertChain.bypass = (m["on"] as? NSNumber)?.boolValue ?? false
            sendState(); save()
        case "plugwin": openWindow()
        default: break
        }
    }

    func set(_ comp: AVAudioUnitComponent?, state: [String: Any]?) {
        let c = insertChain
        window?.close(); window = nil
        guard let comp = comp else {
            c.lock.lock()
            if let u = c.fx { c.engine.disconnectNodeOutput(u); c.engine.detach(u) }
            c.fx = nil; c.comp = nil
            if let s = c.source { c.engine.disconnectNodeOutput(s); c.engine.connect(s, to: c.engine.mainMixerNode, format: format) }
            c.lock.unlock()
            note("removed"); sendState(); save(); return
        }
        loading = true; sendState()
        AVAudioUnit.instantiate(with: comp.audioComponentDescription, options: []) { unit, error in
            Task { @MainActor in
                self.loading = false
                guard let unit = unit else {
                    self.note("could not load \(comp.name): \(error?.localizedDescription ?? "no reason given")")
                    self.toPage(["t": "toast", "text": "Could not load \(comp.name)"]); self.sendState(); return
                }
                unit.auAudioUnit.musicalContextBlock = { tempo, num, den, beatPos, offset, downbeat in
                    var bpm = 120.0, playing: Int32 = 0, beat = 0.0
                    sh_transport(&bpm, &playing, &beat)
                    tempo?.pointee = bpm; num?.pointee = 4; den?.pointee = 4
                    beatPos?.pointee = beat; offset?.pointee = 0; downbeat?.pointee = (beat / 4).rounded(.down) * 4
                    return true
                }
                unit.auAudioUnit.transportStateBlock = { flags, samplePos, cycleStart, cycleEnd in
                    var bpm = 120.0, playing: Int32 = 0, beat = 0.0
                    sh_transport(&bpm, &playing, &beat)
                    flags?.pointee = playing != 0 ? [.moving] : []
                    samplePos?.pointee = beat * 60.0 / max(bpm, 1) * 48000
                    cycleStart?.pointee = 0; cycleEnd?.pointee = 0
                    return true
                }
                if let st = state { unit.auAudioUnit.fullState = st }
                c.locked {
                    if let old = c.fx { c.engine.disconnectNodeOutput(old); c.engine.detach(old) }
                    c.engine.attach(unit)
                    if let s = c.source {
                        c.engine.disconnectNodeOutput(s)
                        c.engine.connect(s, to: unit, format: self.format)
                        c.engine.connect(unit, to: c.engine.mainMixerNode, format: self.format)
                    }
                    c.fx = unit; c.comp = comp
                }
                self.note("\(self.title(comp)) loaded" + (state != nil ? " with its settings" : ""))
                self.sendState(); self.save()
            }
        }
    }

    // ---- the instrument
    func hostBlocks(_ u: AVAudioUnit) {
        u.auAudioUnit.musicalContextBlock = { tempo, num, den, beatPos, offset, downbeat in
            var bpm = 120.0, playing: Int32 = 0, beat = 0.0
            sh_transport(&bpm, &playing, &beat)
            tempo?.pointee = bpm; num?.pointee = 4; den?.pointee = 4
            beatPos?.pointee = beat; offset?.pointee = 0; downbeat?.pointee = (beat / 4).rounded(.down) * 4
            return true
        }
        u.auAudioUnit.transportStateBlock = { flags, samplePos, cycleStart, cycleEnd in
            var bpm = 120.0, playing: Int32 = 0, beat = 0.0
            sh_transport(&bpm, &playing, &beat)
            flags?.pointee = playing != 0 ? [.moving] : []
            samplePos?.pointee = beat * 60.0 / max(bpm, 1) * 48000
            cycleStart?.pointee = 0; cycleEnd?.pointee = 0
            return true
        }
    }
    func setInstrument(_ comp: AVAudioUnitComponent?, state: [String: Any]?) {
        let c = instrChain
        instWindow?.close(); instWindow = nil
        guard let comp = comp else {
            c.locked {
                if let u = c.inst { c.engine.disconnectNodeOutput(u); c.engine.detach(u) }
                c.inst = nil; c.comp = nil; c.sched = nil
            }
            sh_msg("{\"t\":\"set\",\"f\":\"engine\",\"v\":0}")
            note("instrument: INTERNAL (AURA)"); sendInst(); saveInstrument(); return
        }
        instLoading = true; sendInst()
        AVAudioUnit.instantiate(with: comp.audioComponentDescription, options: []) { unit, error in
            Task { @MainActor in
                self.instLoading = false
                guard let unit = unit else {
                    self.note("could not load \(comp.name): \(error?.localizedDescription ?? "no reason given")")
                    self.toPage(["t": "toast", "text": "Could not load \(comp.name)"]); self.sendInst(); return
                }
                self.hostBlocks(unit)
                if let st = state { unit.auAudioUnit.fullState = st }
                c.locked {
                    if let old = c.inst { c.engine.disconnectNodeOutput(old); c.engine.detach(old) }
                    c.engine.attach(unit)
                    c.engine.connect(unit, to: c.engine.mainMixerNode, format: self.format)
                    c.inst = unit; c.comp = comp
                    c.sched = unit.auAudioUnit.scheduleMIDIEventBlock
                }
                sh_msg("{\"t\":\"set\",\"f\":\"engine\",\"v\":1}")
                self.note("instrument: \(self.title(comp)) loaded" + (state != nil ? " with its sound" : "") + (c.sched == nil ? " (NOT playable by MIDI)" : ""))
                self.sendInst(); self.saveInstrument()
            }
        }
    }
    func openInstWindow() {
        let c = instrChain
        guard let u = c.inst else { toPage(["t": "toast", "text": "INTERNAL is selected: its controls are on the panel"]); return }
        if let w = instWindow { w.makeKeyAndOrderFront(nil); return }
        let name = c.comp?.name ?? "Instrument"
        u.auAudioUnit.requestViewController { vc in
            Task { @MainActor in
                guard let vc = vc else { self.toPage(["t": "toast", "text": "\(name) has no window of its own"]); return }
                let w = PlugWindow(contentViewController: vc)
                w.page = self.page
                w.styleMask = [.titled, .closable, .miniaturizable]
                w.title = name + "  ·  Chordially instrument"
                w.isReleasedWhenClosed = false
                w.makeKeyAndOrderFront(nil)
                self.instWindow = w
            }
        }
    }
    func saveInstrument() {
        let c = instrChain
        var o: [String: Any] = [:]
        if let comp = c.comp, let u = c.inst {
            o["id"] = ident(comp); o["name"] = title(comp)
            if let st = u.auAudioUnit.fullState, let d = try? PropertyListSerialization.data(fromPropertyList: st, format: .binary, options: 0) { o["state"] = d.base64EncodedString() }
        }
        if let d = try? JSONSerialization.data(withJSONObject: o) { try? d.write(to: URL(fileURLWithPath: instFile)) }
    }
    func restoreInstrument() {
        guard let d = try? Data(contentsOf: URL(fileURLWithPath: instFile)), let o = (try? JSONSerialization.jsonObject(with: d)) as? [String: Any],
              let id = o["id"] as? String else { sh_msg("{\"t\":\"set\",\"f\":\"engine\",\"v\":0}"); return }
        var st: [String: Any]? = nil
        if let b = o["state"] as? String, let bd = Data(base64Encoded: b) { st = (try? PropertyListSerialization.propertyList(from: bd, options: [], format: nil)) as? [String: Any] }
        if let comp = instruments.first(where: { ident($0) == id }) { setInstrument(comp, state: st) }
        else { note("instrument \(o["name"] as? String ?? id) is not installed any more: INTERNAL"); sh_msg("{\"t\":\"set\",\"f\":\"engine\",\"v\":0}") }
    }

    func openWindow() {
        let c = insertChain
        guard let u = c.fx else { toPage(["t": "toast", "text": "No plugin in the insert"]); return }
        if let w = window { w.makeKeyAndOrderFront(nil); return }
        let name = c.comp?.name ?? "Plugin"
        u.auAudioUnit.requestViewController { vc in
            Task { @MainActor in
                guard let vc = vc else { self.toPage(["t": "toast", "text": "\(name) has no window of its own"]); return }
                let w = PlugWindow(contentViewController: vc)
                w.page = self.page
                w.styleMask = [.titled, .closable, .miniaturizable]
                w.title = name + "  ·  Chordially insert"
                w.isReleasedWhenClosed = false
                w.makeKeyAndOrderFront(nil)
                self.window = w
            }
        }
    }

    // insert.json: {"id","name","byp","state"(base64 plist)}
    func save() {
        let c = insertChain
        var o: [String: Any] = ["byp": c.bypass]
        if let comp = c.comp, let u = c.fx {
            o["id"] = ident(comp); o["name"] = title(comp)
            if let st = u.auAudioUnit.fullState, let d = try? PropertyListSerialization.data(fromPropertyList: st, format: .binary, options: 0) { o["state"] = d.base64EncodedString() }
        }
        if let d = try? JSONSerialization.data(withJSONObject: o) { try? d.write(to: URL(fileURLWithPath: file)) }
    }
    func restore() {
        guard let d = try? Data(contentsOf: URL(fileURLWithPath: file)), let o = (try? JSONSerialization.jsonObject(with: d)) as? [String: Any] else { return }
        insertChain.bypass = (o["byp"] as? Bool) ?? false
        guard let id = o["id"] as? String else { return }
        var st: [String: Any]? = nil
        if let b = o["state"] as? String, let bd = Data(base64Encoded: b) { st = (try? PropertyListSerialization.propertyList(from: bd, options: [], format: nil)) as? [String: Any] }
        if let comp = effects.first(where: { ident($0) == id }) { set(comp, state: st) }
        else { note("\(o["name"] as? String ?? id) is not installed any more") }
    }
    func stopEngines() {
        insertChain.locked { insertChain.ok = false }
        instrChain.locked { instrChain.ok = false }
        insertChain.engine.stop(); instrChain.engine.stop()
    }
    func quit() {
        if let sch = instrChain.sched { var b: (UInt8, UInt8, UInt8) = (0xB0, 123, 0); withUnsafeBytes(of: &b) { p in sch(AUEventSampleTimeImmediate, 0, 3, p.bindMemory(to: UInt8.self).baseAddress!) } }
        save(); saveInstrument(); window?.close(); instWindow?.close()
    }
}
let insertHost = InsertHost()
