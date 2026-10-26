import SwiftUI
import GameController
import CoreHaptics
import UIKit

enum ControllerAction: String, CaseIterable, Codable, Identifiable {
    case confirm, back, pause, handbrake, nitro, camera, reset, lookBack, shiftDown, shiftUp, none
    var id: String { rawValue }
    var title: String {
        switch self {
        case .confirm: return "Onay / Atla (Enter)"
        case .back: return "Geri (Backspace)"
        case .pause: return "Duraklat / Çık (Esc)"
        case .handbrake: return "El freni (Boşluk)"
        case .nitro: return "Nitro"
        case .camera: return "Kamera"
        case .reset: return "Aracı sıfırla"
        case .lookBack: return "Arkaya bak"
        case .shiftDown: return "Vites küçült"
        case .shiftUp: return "Vites yükselt"
        case .none: return "Kapalı"
        }
    }
    var keys: [UInt8] {
        switch self {
        case .confirm: return [UInt8(NFSU2_DIK_RETURN)]
        case .back: return [UInt8(NFSU2_DIK_BACK)]
        case .pause: return [UInt8(NFSU2_DIK_ESCAPE)]
        case .handbrake: return [UInt8(NFSU2_DIK_SPACE)]
        case .nitro: return [UInt8(NFSU2_DIK_LSHIFT), UInt8(NFSU2_DIK_N)]
        case .camera: return [UInt8(NFSU2_DIK_C)]
        case .reset: return [UInt8(NFSU2_DIK_R)]
        case .lookBack: return [UInt8(NFSU2_DIK_B)]
        case .shiftDown: return [UInt8(NFSU2_DIK_Q)]
        case .shiftUp: return [UInt8(NFSU2_DIK_E)]
        case .none: return []
        }
    }
}

struct ControllerPreferences: Codable {
    var steeringDeadzone: Float = 0.08
    var triggerThreshold: Float = 0.08
    var invertSteering = false
    var swapTriggers = false
    var vibration = true
    var inputFeedback = false
    var vibrationStrength: Float = 0.65
    var cross: ControllerAction = .confirm
    var circle: ControllerAction = .handbrake
    var square: ControllerAction = .nitro
    var triangle: ControllerAction = .camera
    var l1: ControllerAction = .handbrake
    var r1: ControllerAction = .nitro
    var l3: ControllerAction = .reset
    var r3: ControllerAction = .lookBack
    var options: ControllerAction = .back

    static func load() -> Self {
        guard let data = UserDefaults.standard.data(forKey: "controller.preferences.v1"),
              var value = try? JSONDecoder().decode(Self.self, from: data) else { return Self() }
        value.steeringDeadzone = value.steeringDeadzone.isFinite ? min(0.8, max(0.02, value.steeringDeadzone)) : 0.08
        value.triggerThreshold = value.triggerThreshold.isFinite ? min(0.95, max(0.02, value.triggerThreshold)) : 0.08
        value.vibrationStrength = value.vibrationStrength.isFinite ? min(1, max(0, value.vibrationStrength)) : 0.65
        return value
    }
}

// Controller engines only: this never substitutes the iPad's impact generator.
// All engine/player operations are serialized away from the render and UI threads.
private final class ControllerHaptics {
    private let queue = DispatchQueue(label: "dev.lyszq.nfsu2.controller-haptics")
    private var engine: CHHapticEngine?
    private var engineRunning = false
    private var impactPlayer: CHHapticPatternPlayer?
    private var continuousPlayer: CHHapticPatternPlayer?
    private var continuousActive = false
    private var continuousRefreshAt: TimeInterval = 0
    private var status: ((String, Bool, Bool) -> Void)?
    private var playbackFailed = false
    private var lastFailureReport: TimeInterval = -10
    private var retryPlaybackAt: TimeInterval = 0
    private var impactStarts: UInt64 = 0
    private var continuousStarts: UInt64 = 0
    private var playbackErrors: UInt64 = 0

    func attach(_ controller: GCController?, status: @escaping (String, Bool, Bool) -> Void) {
        queue.async {
            self.releaseEngine()
            self.status = status
            self.playbackFailed = false
            self.retryPlaybackAt = 0
            guard let controller else { self.report("Kontrolcü bağlı değil"); return }
            guard let haptics = controller.haptics, !haptics.supportedLocalities.isEmpty else {
                self.report("Bu kontrolcü / bağlantı titreşim sunmuyor")
                return
            }
            guard let engine = haptics.createEngine(withLocality: .default) else {
                self.report("Kontrolcü titreşim motoru oluşturulamadı; yeniden denenecek", retryAttachment: true)
                return
            }
            self.engine = engine
            self.engineRunning = false
            engine.isAutoShutdownEnabled = true
            engine.stoppedHandler = { [weak self, weak engine] _ in
                guard let self else { return }
                self.queue.async {
                    guard let engine, self.engine === engine else { return }
                    self.discardPlayers()
                    self.engineRunning = false
                }
            }
            engine.resetHandler = { [weak self, weak engine] in
                guard let self else { return }
                self.queue.async {
                    guard let engine, self.engine === engine else { return }
                    self.discardPlayers()
                    self.engineRunning = false
                    // Restart on the next live request; a reset can arrive while paused.
                }
            }
            self.report("Kontrolcü titreşimi kullanılabilir")
        }
    }

    private func ensureRunningEngine() throws {
        guard let engine = self.engine else {
            throw NSError(domain: "dev.lyszq.nfsu2.haptics", code: -1, userInfo: [NSLocalizedDescriptionKey: "Haptic engine not initialized"])
        }
        if !engineRunning {
            try engine.start()
            engineRunning = true
        }
    }

    func playImpact(intensity: Float, sharpness: Float, durationMs: UInt32, strength: Float) {
        guard strength.isFinite, intensity.isFinite, sharpness.isFinite,
              strength > 0.001, intensity > 0.01 else { return }
        let sampledAt = ProcessInfo.processInfo.systemUptime
        queue.async { [weak self] in
            guard let self else { return }
            let now = ProcessInfo.processInfo.systemUptime
            guard now - sampledAt < 0.3, now >= self.retryPlaybackAt else { return }
            let scaledIntensity = min(max(intensity * strength, 0), 1)
            let clampedSharpness = min(max(sharpness, 0.1), 1.0)
            let durationSec = min(max(Double(durationMs) / 1000.0, 0.06), 0.30)

            let transientEvent = CHHapticEvent(
                eventType: .hapticTransient,
                parameters: [
                    CHHapticEventParameter(parameterID: .hapticIntensity, value: scaledIntensity),
                    CHHapticEventParameter(parameterID: .hapticSharpness, value: clampedSharpness)
                ],
                relativeTime: 0
            )

            let continuousEvent = CHHapticEvent(
                eventType: .hapticContinuous,
                parameters: [
                    CHHapticEventParameter(parameterID: .hapticIntensity, value: scaledIntensity * 0.85),
                    CHHapticEventParameter(parameterID: .hapticSharpness, value: max(clampedSharpness * 0.7, 0.15))
                ],
                relativeTime: 0,
                duration: durationSec
            )

            func playPattern(on engine: CHHapticEngine) throws {
                let pattern = try CHHapticPattern(events: [transientEvent, continuousEvent], parameters: [])
                let player = try engine.makePlayer(with: pattern)
                try? self.impactPlayer?.stop(atTime: CHHapticTimeImmediate)
                self.impactPlayer = player
                try player.start(atTime: CHHapticTimeImmediate)
                self.impactStarts &+= 1
                self.reportRecovery()
            }

            do {
                try self.ensureRunningEngine()
                guard let engine = self.engine else { return }
                try playPattern(on: engine)
            } catch {
                self.handlePlaybackFailure(error, operation: "Darbe titreşimi")
            }
        }
    }

    func updateContinuous(intensity: Float, sharpness: Float, strength: Float) {
        let sampledAt = ProcessInfo.processInfo.systemUptime
        queue.async { [weak self] in
            guard let self else { return }
            let now = ProcessInfo.processInfo.systemUptime
            if !intensity.isFinite || !sharpness.isFinite || !strength.isFinite ||
                intensity <= 0.01 || strength <= 0.001 || now - sampledAt >= 0.3 {
                if self.continuousActive {
                    try? self.continuousPlayer?.stop(atTime: CHHapticTimeImmediate)
                    self.continuousActive = false
                    self.continuousRefreshAt = 0
                }
                return
            }
            guard now >= self.retryPlaybackAt else { return }
            guard !self.continuousActive || now >= self.continuousRefreshAt else { return }

            let scaledIntensity = min(max(intensity * strength, 0), 1)
            let clampedSharpness = min(max(sharpness, 0.05), 1.0)

            do {
                try self.ensureRunningEngine()
                guard let engine = self.engine else { return }

                // Use a fresh finite pattern with its actual event values. This
                // avoids pre-start dynamic parameters and restarting a live player.
                // Players are lightweight; a sustained road effect needs at most
                // five replacements per second, with no permanently looping motor.
                let event = CHHapticEvent(
                    eventType: .hapticContinuous,
                    parameters: [
                        CHHapticEventParameter(parameterID: .hapticIntensity, value: scaledIntensity),
                        CHHapticEventParameter(parameterID: .hapticSharpness, value: clampedSharpness)
                    ],
                    relativeTime: 0,
                    duration: 0.30
                )
                let pattern = try CHHapticPattern(events: [event], parameters: [])
                let player = try engine.makePlayer(with: pattern)
                try? self.continuousPlayer?.stop(atTime: CHHapticTimeImmediate)
                self.continuousPlayer = player
                try player.start(atTime: CHHapticTimeImmediate)
                self.continuousActive = true
                self.continuousRefreshAt = now + 0.20
                self.continuousStarts &+= 1
                self.reportRecovery()
            } catch {
                self.handlePlaybackFailure(error, operation: "Yol titreşimi")
            }
        }
    }

    func pulse(strength: Float) {
        playImpact(intensity: 0.9, sharpness: 0.65, durationMs: 160, strength: strength)
    }

    func stop() {
        queue.async {
            self.stopPlayers()
            // Keep the engine for temporary pauses. An asynchronous engine stop
            // could otherwise finish after the next synchronous start.
        }
    }

    private func discardPlayers() {
        impactPlayer = nil
        continuousPlayer = nil
        continuousActive = false
        continuousRefreshAt = 0
    }

    private func stopPlayers() {
        try? impactPlayer?.stop(atTime: CHHapticTimeImmediate)
        try? continuousPlayer?.stop(atTime: CHHapticTimeImmediate)
        discardPlayers()
    }

    private func handlePlaybackFailure(_ error: Error, operation: String) {
        stopPlayers()
        engineRunning = false
        playbackFailed = true
        playbackErrors &+= 1
        let now = ProcessInfo.processInfo.systemUptime
        retryPlaybackAt = now + 1
        if now - lastFailureReport >= 5 {
            lastFailureReport = now
            report("\(operation) çalınamadı: \(error.localizedDescription); yeniden denenecek")
        }
    }

    private func reportRecovery() {
        if playbackFailed {
            playbackFailed = false
            report("Kontrolcü titreşimi yeniden çalışıyor")
        }
    }

    func readDiagnostics(_ completion: @escaping (UInt64, UInt64, UInt64) -> Void) {
        queue.async {
            let values = (self.impactStarts, self.continuousStarts, self.playbackErrors)
            DispatchQueue.main.async { completion(values.0, values.1, values.2) }
        }
    }

    private func releaseEngine() {
        stopPlayers()
        engineRunning = false
        engine?.stoppedHandler = { _ in }
        engine?.resetHandler = {}
        engine?.stop(completionHandler: nil)
        engine = nil
    }

    private func report(_ message: String, retryAttachment: Bool = false) {
        let callback = status
        let available = engine != nil
        DispatchQueue.main.async { callback?(message, available, retryAttachment) }
    }
}

public final class GameControllerManager: ObservableObject {
    public static let shared = GameControllerManager()
    @Published public private(set) var connectedControllerName: String?
    @Published public private(set) var isControllerConnected = false
    @Published public private(set) var batteryLevel: Float?
    @Published private(set) var hapticsStatus = "Kontrolcü bağlı değil"
    @Published private(set) var supportsControllerHaptics = false
    @Published private(set) var rumbleDiagnostics = "Henüz oyun titreşimi örneği yok"
    @Published private(set) var hapticsPlaybackDiagnostics = "Oynatıcı: 0 darbe · 0 yol · 0 hata"
    @Published var preferences = ControllerPreferences.load() {
        didSet {
            if let data = try? JSONEncoder().encode(preferences) {
                UserDefaults.standard.set(data, forKey: "controller.preferences.v1")
            }
            releaseInput()
            armNeutralGate()
            if !preferences.vibration { haptics.stop() }
            sample()
        }
    }
    private var controller: GCController?
    private var keys = Set<UInt8>()
    private var suppressedKeys = Set<UInt8>()
    private var inputLifecycle = GameInputLifecycle()
    private var menuOpen: Bool { inputLifecycle.settingsOpen }
    private var active: Bool { inputLifecycle.sessionActive && inputLifecycle.applicationActive }
    private var lastImpactId: UInt32 = 0
    private var rumblePolls: UInt64 = 0
    private var receivedImpacts: UInt64 = 0
    private var receivedContinuousTicks: UInt64 = 0
    private var retryHapticsAttachment = false
    private var nextAttachmentRetryAt: TimeInterval = 0
    private let haptics = ControllerHaptics()
    private var observers: [NSObjectProtocol] = []
    private var batteryTimer: Timer?
    private var rumbleDiagnosticsTimer: Timer?

    private init() {
        inputLifecycle.applicationActive = UIApplication.shared.applicationState == .active
        for name in [Notification.Name.GCControllerDidConnect, .GCControllerDidDisconnect,
                     .GCControllerDidBecomeCurrent, .GCControllerDidStopBeingCurrent] {
            observers.append(NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                self?.checkForExistingControllers()
            })
        }
        batteryTimer = Timer.scheduledTimer(withTimeInterval: 15, repeats: true) { [weak self] _ in self?.refreshBattery() }
        let timer = Timer(timeInterval: 1, repeats: true) { [weak self] _ in self?.refreshRumbleDiagnostics() }
        RunLoop.main.add(timer, forMode: .common)
        rumbleDiagnosticsTimer = timer
        checkForExistingControllers()
    }

    public func checkForExistingControllers(forceRebind: Bool = false) {
        let available = GCController.controllers()
        let next = GCController.current.flatMap { current in available.first { $0 === current } } ?? available.first
        if controller === next && !forceRebind { refreshBattery(); return }
        controller?.extendedGamepad?.valueChangedHandler = nil
        controller?.microGamepad?.valueChangedHandler = nil
        releaseInput()
        controller = next
        armNeutralGate()
        next?.handlerQueue = .main
        isControllerConnected = next != nil
        connectedControllerName = next?.vendorName ?? (next == nil ? nil : "Kablosuz oyun kolu")
        supportsControllerHaptics = false
        retryHapticsAttachment = false
        refreshBattery()
        attachHaptics(to: next)
        if let pad = next?.extendedGamepad {
            pad.valueChangedHandler = { [weak self, weak next] _, _ in
                guard let self, let next, self.controller === next else { return }
                self.sample()
            }
        } else if let pad = next?.microGamepad {
            pad.valueChangedHandler = { [weak self, weak next] _, _ in
                guard let self, let next, self.controller === next else { return }
                self.sample()
            }
        }
        sample()
    }

    func setActive(_ value: Bool) {
        inputLifecycle.sessionActive = value
        applyInputLifecycle()
    }

    func setApplicationActive(_ value: Bool) {
        guard inputLifecycle.applicationActive != value else { return }
        inputLifecycle.applicationActive = value
        if value { checkForExistingControllers(forceRebind: true) }
        applyInputLifecycle()
    }

    private func applyInputLifecycle() {
        inputLifecycle.applyToBridge()
        releaseInput()
        armNeutralGate()
        haptics.stop()
        sample()
    }

    func setMenuOpen(_ value: Bool) {
        inputLifecycle.settingsOpen = value
        applyInputLifecycle()
        refreshBattery()
    }

    func tryVibration() {
        guard supportsControllerHaptics, preferences.vibration else { return }
        haptics.pulse(strength: preferences.vibrationStrength)
    }

    public func pollRumbleTick() {
        var snapshot = NFSU2RumbleSnapshot()
        _ = nfsu2_input_poll_rumble(&snapshot)
        rumblePolls &+= 1
        if snapshot.impact_intensity > 0.01 { receivedImpacts &+= 1 }
        if snapshot.continuous_intensity > 0.01 { receivedContinuousTicks &+= 1 }

        let isActive = self.active && !self.menuOpen
        let hasConnectedController = self.isControllerConnected && self.controller != nil
        let hapticsAvailable = self.supportsControllerHaptics

        if isActive && preferences.vibration && hasConnectedController && hapticsAvailable {
            if snapshot.impact_id != lastImpactId && snapshot.impact_intensity > 0.01 {
                lastImpactId = snapshot.impact_id
                haptics.playImpact(
                    intensity: snapshot.impact_intensity,
                    sharpness: snapshot.impact_sharpness,
                    durationMs: snapshot.impact_duration_ms,
                    strength: preferences.vibrationStrength
                )
            }
            haptics.updateContinuous(
                intensity: snapshot.continuous_intensity,
                sharpness: snapshot.continuous_sharpness,
                strength: preferences.vibrationStrength
            )
        } else {
            lastImpactId = snapshot.impact_id
            haptics.updateContinuous(intensity: 0, sharpness: 0, strength: 0)
        }
    }

    private func attachHaptics(to target: GCController?) {
        haptics.attach(target) { [weak self, weak target] message, available, retryAttachment in
            guard let self, self.controller === target else { return }
            self.hapticsStatus = message
            self.supportsControllerHaptics = available
            self.retryHapticsAttachment = retryAttachment
            self.nextAttachmentRetryAt = ProcessInfo.processInfo.systemUptime + 5
        }
    }

    private func refreshRumbleDiagnostics() {
        var producer = NFSU2RumbleDiagnostics()
        nfsu2_input_get_rumble_diagnostics(&producer)
        let gate: String
        switch producer.last_gate {
        case 1: gate = "Araç adresi geçersiz"
        case 2: gate = "Yerel olmayan araç"
        case 3: gate = "Yarış duraklatılmış / etkin değil"
        case 4: gate = "Yerel oyuncu aktif"
        default: gate = "Henüz fizik çağrısı yok"
        }
        rumbleDiagnostics = "\(gate)\nFizik: \(producer.hook_calls) çağrı · \(producer.local_samples) yerel · \(producer.gated_samples) engellenen\nÜretim: \(producer.impact_reports) darbe · \(producer.continuous_reports) yol\nKöprü: \(rumblePolls) okuma · \(receivedImpacts) darbe · \(receivedContinuousTicks) yol örneği"
        haptics.readDiagnostics { [weak self] impacts, continuous, errors in
            self?.hapticsPlaybackDiagnostics = "Oynatıcı: \(impacts) darbe · \(continuous) yol başlangıcı · \(errors) hata"
        }
        // Retry transient engine-creation failures; unsupported hardware does not
        // request a retry and must not be reattached on every display tick.
        if retryHapticsAttachment, active, !menuOpen, preferences.vibration,
           let controller, ProcessInfo.processInfo.systemUptime >= nextAttachmentRetryAt {
            retryHapticsAttachment = false
            attachHaptics(to: controller)
        }
    }

    func suspendRumble() {
        nfsu2_input_clear_rumble()
        haptics.stop()
    }

    private func refreshBattery() {
        let value = controller?.battery?.batteryLevel
        batteryLevel = value.flatMap { $0 >= 0 && $0 <= 1 ? $0 : nil }
    }

    private func releaseInput() {
        keys.removeAll()
        nfsu2_input_clear_controller()
    }

    private func armNeutralGate() {
        suppressedKeys = rawPressedKeys()
    }

    private func rawPressedKeys() -> Set<UInt8> {
        var pressed = Set<UInt8>()
        func add(_ down: Bool, _ codes: [UInt8]) {
            if down { pressed.formUnion(codes) }
        }
        let p = preferences
        if let pad = controller?.extendedGamepad {
            let x = pad.leftThumbstick.xAxis.value * (p.invertSteering ? -1 : 1)
            add(x < -p.steeringDeadzone, [UInt8(NFSU2_DIK_LEFT), UInt8(NFSU2_DIK_A)])
            add(x > p.steeringDeadzone, [UInt8(NFSU2_DIK_RIGHT), UInt8(NFSU2_DIK_D)])
            let gas = p.swapTriggers ? pad.leftTrigger.value : pad.rightTrigger.value
            let brake = p.swapTriggers ? pad.rightTrigger.value : pad.leftTrigger.value
            add(gas > p.triggerThreshold, [UInt8(NFSU2_DIK_UP), UInt8(NFSU2_DIK_W)])
            add(brake > p.triggerThreshold, [UInt8(NFSU2_DIK_DOWN), UInt8(NFSU2_DIK_S)])
            add(pad.dpad.left.isPressed, [UInt8(NFSU2_DIK_LEFT), UInt8(NFSU2_DIK_A)])
            add(pad.dpad.right.isPressed, [UInt8(NFSU2_DIK_RIGHT), UInt8(NFSU2_DIK_D)])
            add(pad.dpad.up.isPressed, [UInt8(NFSU2_DIK_UP), UInt8(NFSU2_DIK_W)])
            add(pad.dpad.down.isPressed, [UInt8(NFSU2_DIK_DOWN), UInt8(NFSU2_DIK_S)])
            for (button, action) in [(pad.buttonA, p.cross), (pad.buttonB, p.circle),
                                     (pad.buttonX, p.square), (pad.buttonY, p.triangle),
                                     (pad.leftShoulder, p.l1), (pad.rightShoulder, p.r1)] {
                add(button.isPressed, action.keys)
            }
            add(pad.buttonOptions?.isPressed == true, p.options.keys)
            add(pad.buttonMenu.isPressed, ControllerAction.pause.keys)
            add(pad.leftThumbstickButton?.isPressed == true, p.l3.keys)
            add(pad.rightThumbstickButton?.isPressed == true, p.r3.keys)
        } else if let pad = controller?.microGamepad {
            add(pad.dpad.xAxis.value < -p.steeringDeadzone, [UInt8(NFSU2_DIK_LEFT)])
            add(pad.dpad.xAxis.value > p.steeringDeadzone, [UInt8(NFSU2_DIK_RIGHT)])
            add(pad.dpad.yAxis.value > p.steeringDeadzone, [UInt8(NFSU2_DIK_UP)])
            add(pad.dpad.yAxis.value < -p.steeringDeadzone, [UInt8(NFSU2_DIK_DOWN)])
            add(pad.buttonA.isPressed, ControllerAction.confirm.keys)
            add(pad.buttonX.isPressed, ControllerAction.back.keys)
            add(pad.buttonMenu.isPressed, ControllerAction.pause.keys)
        }
        return pressed
    }

    private func sample() {
        guard active, !menuOpen else { releaseInput(); return }
        let rawPressed = rawPressedKeys()
        suppressedKeys.formIntersection(rawPressed)
        let pressed = rawPressed.subtracting(suppressedKeys)
        let p = preferences
        let newKeys = pressed.subtracting(keys)
        for key in keys.subtracting(pressed) { nfsu2_input_set_controller_key(key, false) }
        for key in newKeys { nfsu2_input_set_controller_key(key, true) }
        keys = pressed
        let feedbackKeys = Set(ControllerAction.allCases.flatMap(\.keys))
        if p.vibration && p.inputFeedback && !newKeys.intersection(feedbackKeys).isEmpty {
            haptics.pulse(strength: p.vibrationStrength)
        }
    }
}

struct GameSettingsPanel: View {
    @Environment(\.dismiss) private var dismiss
    @ObservedObject var controller = GameControllerManager.shared
    @Binding var touchMode: Int
    @Binding var useSlider: Bool
    @Binding var manualGears: Bool
    @Binding var showTopButtons: Bool
    let runtimeStatus: String
    let runtimeLogURL: URL?

    var body: some View {
        NavigationStack {
            Form {
                Section("Çalışma durumu") {
                    Text(runtimeStatus).font(.footnote).textSelection(.enabled)
                        .accessibilityIdentifier("full-runtime-status")
                    if let url = runtimeLogURL {
                        ShareLink(item: url) { Label("Çalışma günlüğünü paylaş", systemImage: "square.and.arrow.up") }
                    }
                }
                Section("Oyun") {
                    Picker("Dokunmatik kontroller", selection: $touchMode) {
                        Text("Otomatik").tag(0)
                        Text("Göster").tag(1)
                        Text("Gizle").tag(2)
                    }
                    Toggle("Direksiyon kaydırıcısı", isOn: $useSlider)
                    Toggle("Manuel vites düğmeleri", isOn: $manualGears)
                    Toggle("Üst kısayol tuşlarını göster", isOn: $showTopButtons)
                }
                Section("Kontrolcü") {
                    Text(controller.connectedControllerName ?? "Kontrolcü bağlı değil")
                    if let battery = controller.batteryLevel {
                        Text("Pil: %\(Int((battery * 100).rounded()))")
                    } else if controller.isControllerConnected {
                        Text("Pil bilgisi alınamıyor").foregroundStyle(.secondary)
                    }
                    Slider(value: $controller.preferences.steeringDeadzone, in: 0.02...0.8, step: 0.01) {
                        Text("Direksiyon ölü bölgesi")
                    } minimumValueLabel: { Text("%2") } maximumValueLabel: { Text("%80") }
                    Text("Direksiyon ölü bölgesi: %\(Int((controller.preferences.steeringDeadzone * 100).rounded()))")
                    Slider(value: $controller.preferences.triggerThreshold, in: 0.02...0.95, step: 0.01) {
                        Text("Tetik basma eşiği")
                    }
                    Text("Tetik basma eşiği: %\(Int((controller.preferences.triggerThreshold * 100).rounded()))")
                    Text("Oyun kolu direksiyon ve pedalları klavye tuşları olarak kullanır; eşik aşıldığında tuş basılır.")
                        .font(.footnote).foregroundStyle(.secondary)
                    Toggle("Direksiyon yönünü ters çevir", isOn: $controller.preferences.invertSteering)
                    Toggle("Gaz ve fren tetiklerini değiştir", isOn: $controller.preferences.swapTriggers)
                }
                Section("Tuş eşlemeleri") {
                    mapping("Çarpı / A", $controller.preferences.cross)
                    mapping("Daire / B", $controller.preferences.circle)
                    mapping("Kare / X", $controller.preferences.square)
                    mapping("Üçgen / Y", $controller.preferences.triangle)
                    mapping("L1 / LB", $controller.preferences.l1)
                    mapping("R1 / RB", $controller.preferences.r1)
                    mapping("L3", $controller.preferences.l3)
                    mapping("R3", $controller.preferences.r3)
                    mapping("Share / View", $controller.preferences.options)
                    Text("Options / Menu: Duraklat (Esc). Varsayılan: Çarpı / A → Onay / Atla (Enter), Share / View → Geri (Backspace).")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                Section("Kontrolcü titreşimi") {
                    Text(controller.hapticsStatus)
                    Toggle("Titreşim", isOn: $controller.preferences.vibration)
                    Toggle("Aksiyon tuşlarında kısa titreşim", isOn: $controller.preferences.inputFeedback)
                        .disabled(!controller.preferences.vibration)
                    Slider(value: $controller.preferences.vibrationStrength, in: 0...1, step: 0.05) {
                        Text("Titreşim şiddeti")
                    }.disabled(!controller.preferences.vibration)
                    Text("Şiddet: %\(Int((controller.preferences.vibrationStrength * 100).rounded()))")
                    Button("Kontrolcüde kısa titreşim dene") { controller.tryVibration() }
                        .disabled(!controller.supportsControllerHaptics || !controller.preferences.vibration)
                    Text("Çarpışma darbeleri, duvar sürtmesi, yol yüzeyi, lastik kayması ve tuş geri bildirim titreşimlerini kontrol eder.")
                        .font(.footnote).foregroundStyle(.secondary)
                    DisclosureGroup("Titreşim tanısı") {
                        Text(controller.rumbleDiagnostics)
                        Text(controller.hapticsPlaybackDiagnostics)
                        Text("Sayaçlar bu açılış içindir. Oynatıcı sayacı, Core Haptics çağrısının kabulünü gösterir; kolda hissedildiğini doğrulamaz.")
                    }
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    Button("Kontrolcü ayarlarını varsayılana döndür") { controller.preferences = ControllerPreferences() }
                }
            }
            .navigationTitle("Ayarlar")
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Bitti") { dismiss() } } }
        }
    }

    private func mapping(_ title: String, _ action: Binding<ControllerAction>) -> some View {
        Picker(title, selection: action) {
            ForEach(ControllerAction.allCases) { value in Text(value.title).tag(value) }
        }
    }
}
