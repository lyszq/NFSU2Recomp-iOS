import SwiftUI
import MetalKit
import GameController
import QuartzCore
import UniformTypeIdentifiers

// MARK: - Dokunsal Titreşim (Haptics) Yöneticisi

private final class HapticFeedbackEngine {
    static let shared = HapticFeedbackEngine()
    private let impactLight = UIImpactFeedbackGenerator(style: .light)
    private let impactRigid = UIImpactFeedbackGenerator(style: .rigid)
    
    private init() {
        impactLight.prepare()
        impactRigid.prepare()
    }
    
    func tapLight() {
        impactLight.impactOccurred()
    }
    
    func tapRigid() {
        impactRigid.impactOccurred()
    }
}

// MARK: - Çoklu Dokunma Uyumlu Aksiyon Düğmesi (Zero-Latency Multi-Touch Button)

public struct MultiTouchKeyButton: View {
    let icon: String
    let label: String
    let dikCode: UInt8
    let secondaryDik: UInt8?
    let tintColor: Color
    let size: CGSize
    
    @State private var isPressed: Bool = false
    @State private var pressStartTime: Double = 0.0
    @State private var pressToken: UInt64 = 0
    
    public init(
        icon: String,
        label: String,
        dikCode: UInt8,
        secondaryDik: UInt8? = nil,
        tintColor: Color = .mint,
        size: CGSize = CGSize(width: 68, height: 68)
    ) {
        self.icon = icon
        self.label = label
        self.dikCode = dikCode
        self.secondaryDik = secondaryDik
        self.tintColor = tintColor
        self.size = size
    }
    
    public var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 18)
                .fill(tintColor.opacity(isPressed ? 0.65 : 0.22))
                .background(
                    RoundedRectangle(cornerRadius: 18)
                        .fill(Color.black.opacity(0.40))
                )
                .overlay(
                    RoundedRectangle(cornerRadius: 18)
                        .stroke(tintColor.opacity(isPressed ? 1.0 : 0.50), lineWidth: isPressed ? 2.5 : 1.5)
                )
                .shadow(color: isPressed ? tintColor.opacity(0.8) : .clear, radius: 10)
            
            VStack(spacing: 3) {
                Image(systemName: icon)
                    .font(.system(size: size.width * 0.32, weight: .bold))
                if !label.isEmpty {
                    Text(label)
                        .font(.system(size: max(9, size.width * 0.14), weight: .heavy, design: .monospaced))
                        .lineLimit(1)
                        .minimumScaleFactor(0.7)
                        .padding(.horizontal, 2)
                }
            }
            .foregroundStyle(.white)
        }
        .frame(width: size.width, height: size.height)
        .scaleEffect(isPressed ? 0.92 : 1.0)
        .animation(.easeOut(duration: 0.06), value: isPressed)
        .gesture(
            DragGesture(minimumDistance: 0, coordinateSpace: .local)
                .onChanged { _ in
                    if !isPressed {
                        isPressed = true
                        pressStartTime = CACurrentMediaTime()
                        pressToken &+= 1
                        nfsu2_input_set_key(dikCode, true)
                        if let sec = secondaryDik {
                            nfsu2_input_set_key(sec, true)
                        }
                        HapticFeedbackEngine.shared.tapLight()
                    }
                }
                .onEnded { _ in
                    isPressed = false
                    let elapsed = CACurrentMediaTime() - pressStartTime
                    let minHoldDuration: Double = 0.050 // 50ms (garantili 3-4 DirectInput polling karesi)
                    let code = dikCode
                    let sec = secondaryDik
                    let token = pressToken
                    
                    if elapsed < minHoldDuration {
                        DispatchQueue.main.asyncAfter(deadline: .now() + (minHoldDuration - elapsed)) {
                            if !isPressed && pressToken == token {
                                nfsu2_input_set_key(code, false)
                                if let s = sec {
                                    nfsu2_input_set_key(s, false)
                                }
                            }
                        }
                    } else {
                        nfsu2_input_set_key(code, false)
                        if let s = sec {
                            nfsu2_input_set_key(s, false)
                        }
                    }
                }
        )
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(label.isEmpty ? icon : label)
        .accessibilityAddTraits(.isButton)
        .accessibilityAction {
            nfsu2_input_set_key(dikCode, true)
            if let secondaryDik { nfsu2_input_set_key(secondaryDik, true) }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.08) {
                nfsu2_input_set_key(dikCode, false)
                if let secondaryDik { nfsu2_input_set_key(secondaryDik, false) }
            }
        }
        .onDisappear {
            isPressed = false
            nfsu2_input_set_key(dikCode, false)
            if let secondaryDik { nfsu2_input_set_key(secondaryDik, false) }
        }
    }
}

// MARK: - Yarış Pedalı Bileşeni (Gaz ve Fren)

public struct RacingPedalView: View {
    let label: String
    let subLabel: String
    let dikCode: UInt8
    let secondaryDik: UInt8?
    let tintColor: Color
    let size: CGSize
    
    @State private var isPressed: Bool = false
    @State private var pressStartTime: Double = 0.0
    @State private var pressToken: UInt64 = 0
    
    public init(
        label: String,
        subLabel: String,
        dikCode: UInt8,
        secondaryDik: UInt8? = nil,
        tintColor: Color = .mint,
        size: CGSize = CGSize(width: 76, height: 104)
    ) {
        self.label = label
        self.subLabel = subLabel
        self.dikCode = dikCode
        self.secondaryDik = secondaryDik
        self.tintColor = tintColor
        self.size = size
    }
    
    public var body: some View {
        ZStack {
            // Pedal Arka Planı
            RoundedRectangle(cornerRadius: 16)
                .fill(tintColor.opacity(isPressed ? 0.70 : 0.25))
                .background(
                    RoundedRectangle(cornerRadius: 16)
                        .fill(Color.black.opacity(0.50))
                )
                .overlay(
                    RoundedRectangle(cornerRadius: 16)
                        .stroke(tintColor.opacity(isPressed ? 1.0 : 0.55), lineWidth: isPressed ? 3 : 1.8)
                )
                .shadow(color: isPressed ? tintColor.opacity(0.85) : .clear, radius: 12)
            
            // Pedal Çizgileri / Izgara Görünümü
            VStack(spacing: 6) {
                ForEach(0..<4) { _ in
                    Capsule()
                        .fill(Color.white.opacity(isPressed ? 0.60 : 0.25))
                        .frame(width: size.width * 0.65, height: 3.5)
                }
                
                Spacer().frame(height: 4)
                
                Text(label)
                    .font(.system(size: 13, weight: .black, design: .monospaced))
                    .foregroundStyle(.white)
                
                if !subLabel.isEmpty {
                    Text(subLabel)
                        .font(.system(size: 9, weight: .bold, design: .monospaced))
                        .foregroundStyle(tintColor.opacity(0.9))
                }
            }
            .padding(.vertical, 8)
        }
        .frame(width: size.width, height: size.height)
        .scaleEffect(isPressed ? 0.94 : 1.0)
        .animation(.easeOut(duration: 0.05), value: isPressed)
        .gesture(
            DragGesture(minimumDistance: 0, coordinateSpace: .local)
                .onChanged { _ in
                    if !isPressed {
                        isPressed = true
                        pressStartTime = CACurrentMediaTime()
                        pressToken &+= 1
                        nfsu2_input_set_key(dikCode, true)
                        if let sec = secondaryDik {
                            nfsu2_input_set_key(sec, true)
                        }
                        HapticFeedbackEngine.shared.tapRigid()
                    }
                }
                .onEnded { _ in
                    isPressed = false
                    let elapsed = CACurrentMediaTime() - pressStartTime
                    let minHoldDuration: Double = 0.050 // 50ms
                    let code = dikCode
                    let sec = secondaryDik
                    let token = pressToken
                    
                    if elapsed < minHoldDuration {
                        DispatchQueue.main.asyncAfter(deadline: .now() + (minHoldDuration - elapsed)) {
                            if !isPressed && pressToken == token {
                                nfsu2_input_set_key(code, false)
                                if let s = sec {
                                    nfsu2_input_set_key(s, false)
                                }
                            }
                        }
                    } else {
                        nfsu2_input_set_key(code, false)
                        if let s = sec {
                            nfsu2_input_set_key(s, false)
                        }
                    }
                }
        )
        .onDisappear {
            isPressed = false
            nfsu2_input_set_key(dikCode, false)
            if let secondaryDik { nfsu2_input_set_key(secondaryDik, false) }
        }
    }
}

// MARK: - Analog Direksiyon Kaydırıcısı (Steer Slider)

public struct AnalogSteerSlider: View {
    let width: CGFloat
    let height: CGFloat
    
    @State private var dragOffset: CGFloat = 0.0
    @State private var isDragging: Bool = false
    
    private var maxRange: CGFloat {
        return (width - height) / 2.0
    }
    
    public init(width: CGFloat = 180, height: CGFloat = 58) {
        self.width = width
        self.height = height
    }
    
    public var body: some View {
        let normalizedValue = maxRange > 0 ? (dragOffset / maxRange) : 0.0
        
        ZStack {
            // Kaydırıcı Rayı (Track)
            RoundedRectangle(cornerRadius: height / 2)
                .fill(Color.black.opacity(0.45))
                .overlay(
                    RoundedRectangle(cornerRadius: height / 2)
                        .stroke(
                            LinearGradient(
                                colors: [
                                    .cyan.opacity(normalizedValue < -0.1 ? 0.9 : 0.3),
                                    .white.opacity(0.3),
                                    .cyan.opacity(normalizedValue > 0.1 ? 0.9 : 0.3)
                                ],
                                startPoint: .leading,
                                endPoint: .trailing
                            ),
                            lineWidth: 2
                        )
                )
            
            // Merkez Noktası İşareti
            Rectangle()
                .fill(Color.white.opacity(0.40))
                .frame(width: 2, height: height * 0.45)
            
            // Sol ve Sağ Yön Göstergeleri
            HStack {
                Image(systemName: "chevron.left.2")
                    .font(.system(size: 13, weight: .bold))
                    .foregroundStyle(normalizedValue < -0.15 ? Color.cyan : Color.white.opacity(0.35))
                    .padding(.leading, 12)
                
                Spacer()
                
                Image(systemName: "chevron.right.2")
                    .font(.system(size: 13, weight: .bold))
                    .foregroundStyle(normalizedValue > 0.15 ? Color.cyan : Color.white.opacity(0.35))
                    .padding(.trailing, 12)
            }
            
            // Hareketli Direksiyon Topuzu (Thumb Knob)
            Circle()
                .fill(
                    LinearGradient(
                        colors: isDragging ? [.cyan, .blue] : [.white.opacity(0.9), .gray.opacity(0.7)],
                        startPoint: .topLeading,
                        endPoint: .bottomTrailing
                    )
                )
                .frame(width: height - 8, height: height - 8)
                .overlay(
                    Circle()
                        .stroke(Color.white, lineWidth: 2)
                )
                .shadow(color: isDragging ? .cyan.opacity(0.8) : .clear, radius: 8)
                .offset(x: dragOffset)
        }
        .frame(width: width, height: height)
        .gesture(
            DragGesture(minimumDistance: 0, coordinateSpace: .local)
                .onChanged { gesture in
                    isDragging = true
                    let rawX = gesture.location.x - (width / 2.0)
                    let clampedX = min(max(rawX, -maxRange), maxRange)
                    dragOffset = clampedX
                    
                    let norm = Float(clampedX / maxRange)
                    nfsu2_input_set_steering(norm)
                }
                .onEnded { _ in
                    isDragging = false
                    // Yaylanarak merkeze dön (Spring Return to Center)
                    withAnimation(.spring(response: 0.18, dampingFraction: 0.70)) {
                        dragOffset = 0.0
                    }
                    nfsu2_input_set_steering(0.0)
                    HapticFeedbackEngine.shared.tapLight()
                }
        )
    }
}

// MARK: - Dokunmatik Kontroller Arayüzü (HUD Katmanı)

public struct OnScreenControlsOverlay: View {
    @Binding var showControls: Bool
    @AppStorage("controls.useSlider") private var useAnalogSlider = true
    @AppStorage("controls.manualGears") private var showManualGears = false
    let openSettings: () -> Void
    
    public init(showControls: Binding<Bool>, openSettings: @escaping () -> Void) {
        self._showControls = showControls
        self.openSettings = openSettings
    }
    
    private var isPad: Bool {
        UIDevice.current.userInterfaceIdiom == .pad
    }
    
    public var body: some View {
        GeometryReader { geo in
            let hPad: CGFloat = isPad ? 36 : 18
            let bPad: CGFloat = isPad ? 32 : 14
            
            let steerWidth: CGFloat = isPad ? 210 : 160
            let steerHeight: CGFloat = isPad ? 62 : 52
            let dpadSize: CGSize = isPad ? CGSize(width: 80, height: 76) : CGSize(width: 66, height: 66)
            let gasSize: CGSize = isPad ? CGSize(width: 80, height: 114) : CGSize(width: 72, height: 98)
            let brakeSize: CGSize = isPad ? CGSize(width: 74, height: 94) : CGSize(width: 66, height: 82)
            let actionSize: CGSize = isPad ? CGSize(width: 72, height: 62) : CGSize(width: 62, height: 54)
            let topBtnSize: CGSize = isPad ? CGSize(width: 68, height: 44) : CGSize(width: 56, height: 40)
            
            VStack {
                // ÜST BÖLÜM: Kamera, Harita, Sıfırla, Kontrolcü Durumu, Menü
                HStack(spacing: isPad ? 12 : 8) {
                    // Kamera Görünümü (C)
                    MultiTouchKeyButton(
                        icon: "camera.viewfinder",
                        label: "KAMERA",
                        dikCode: UInt8(NFSU2_DIK_C),
                        tintColor: .cyan,
                        size: topBtnSize
                    )
                    
                    // Arkaya Bak (B)
                    MultiTouchKeyButton(
                        icon: "eye.fill",
                        label: "ARKA",
                        dikCode: UInt8(NFSU2_DIK_B),
                        tintColor: .teal,
                        size: topBtnSize
                    )
                    
                    // Aracı Sıfırla (R)
                    MultiTouchKeyButton(
                        icon: "arrow.counterclockwise.circle",
                        label: "SIFIRLA",
                        dikCode: UInt8(NFSU2_DIK_R),
                        tintColor: .orange,
                        size: topBtnSize
                    )
                    
                    Spacer()
                    
                    MultiTouchKeyButton(
                        icon: "checkmark", label: "ONAY / ATLA",
                        dikCode: UInt8(NFSU2_DIK_RETURN), tintColor: .mint,
                        size: topBtnSize
                    ).accessibilityLabel("Onay veya atla, Enter")
                    MultiTouchKeyButton(
                        icon: "arrow.uturn.backward", label: "GERİ",
                        dikCode: UInt8(NFSU2_DIK_BACK), tintColor: .white,
                        size: topBtnSize
                    ).accessibilityLabel("Geri, Backspace")
                    
                    // Duraklat / Menü (ESC)
                    MultiTouchKeyButton(
                        icon: "pause.fill",
                        label: "ESC",
                        dikCode: UInt8(NFSU2_DIK_ESCAPE),
                        tintColor: .yellow,
                        size: topBtnSize
                    )
                    Button(action: openSettings) {
                        Image(systemName: "gearshape.fill")
                            .frame(width: 44, height: 44)
                            .foregroundStyle(.white)
                            .background(.ultraThinMaterial, in: Circle())
                    }
                    .accessibilityLabel("Oyun ve kontrolcü ayarları")
                    .accessibilityIdentifier("game-settings")
                }
                .padding(.horizontal, hPad)
                .padding(.top, isPad ? 16 : 8)
                
                Spacer()
                
                // ALT BÖLÜM: Sol Direksiyon Kontrolleri & Sağ Pedallar / Nitro
                if showControls {
                    HStack(alignment: .bottom) {
                        // --- SOL BÖLGE: DİREKSİYON & VİTES ---
                        VStack(alignment: .leading, spacing: 10) {
                            // Manuel Vites Düğmeleri (Q: Vites-, E: Vites+)
                            if showManualGears {
                                HStack(spacing: 8) {
                                    MultiTouchKeyButton(
                                        icon: "arrow.down.square",
                                        label: "V-",
                                        dikCode: UInt8(NFSU2_DIK_Q),
                                        tintColor: .indigo,
                                        size: CGSize(width: 50, height: 42)
                                    )
                                    MultiTouchKeyButton(
                                        icon: "arrow.up.square",
                                        label: "V+",
                                        dikCode: UInt8(NFSU2_DIK_E),
                                        tintColor: .indigo,
                                        size: CGSize(width: 50, height: 42)
                                    )
                                }
                            }
                            
                            if useAnalogSlider {
                                // 1. Analog Direksiyon Kaydırıcısı (Steer Slider)
                                AnalogSteerSlider(width: steerWidth, height: steerHeight)
                            } else {
                                // 2. Dijital D-Pad Tuşları (Sol / Sağ)
                                HStack(spacing: isPad ? 14 : 10) {
                                    MultiTouchKeyButton(
                                        icon: "arrow.left",
                                        label: "SOL",
                                        dikCode: UInt8(NFSU2_DIK_LEFT),
                                        secondaryDik: UInt8(NFSU2_DIK_A),
                                        tintColor: .blue,
                                        size: dpadSize
                                    )
                                    
                                    MultiTouchKeyButton(
                                        icon: "arrow.right",
                                        label: "SAĞ",
                                        dikCode: UInt8(NFSU2_DIK_RIGHT),
                                        secondaryDik: UInt8(NFSU2_DIK_D),
                                        tintColor: .blue,
                                        size: dpadSize
                                    )
                                }
                            }
                        }
                        .padding(.leading, hPad)
                        .padding(.bottom, bPad)
                        
                        Spacer()
                        
                        // --- SAĞ BÖLGE: PEDALLAR, EL FRENİ VE NOS ---
                        HStack(alignment: .bottom, spacing: isPad ? 16 : 12) {
                            // İkincil Eylemler Sütunu: NOS (Shift) & El Freni (Space)
                            VStack(spacing: isPad ? 12 : 9) {
                                // NOS / Nitro (Sol Shift / N)
                                MultiTouchKeyButton(
                                    icon: "flame.fill",
                                    label: "NOS",
                                    dikCode: UInt8(NFSU2_DIK_LSHIFT),
                                    secondaryDik: UInt8(NFSU2_DIK_N),
                                    tintColor: .purple,
                                    size: actionSize
                                )
                                
                                // El Freni / Drift (Boşluk / Space)
                                MultiTouchKeyButton(
                                    icon: "p.circle.fill",
                                    label: "E.FREN",
                                    dikCode: UInt8(NFSU2_DIK_SPACE),
                                    tintColor: .orange,
                                    size: actionSize
                                )
                            }
                            
                            // Ana Pedallar: Fren (Aşağı Ok / S) & Gaz (Yukarı Ok / W)
                            HStack(spacing: isPad ? 14 : 10) {
                                RacingPedalView(
                                    label: "FREN",
                                    subLabel: "GERİ",
                                    dikCode: UInt8(NFSU2_DIK_DOWN),
                                    secondaryDik: UInt8(NFSU2_DIK_S),
                                    tintColor: .red,
                                    size: brakeSize
                                )
                                
                                RacingPedalView(
                                    label: "GAZ",
                                    subLabel: "İLERİ",
                                    dikCode: UInt8(NFSU2_DIK_UP),
                                    secondaryDik: UInt8(NFSU2_DIK_W),
                                    tintColor: .mint,
                                    size: gasSize
                                )
                            }
                        }
                        .padding(.trailing, hPad)
                        .padding(.bottom, bPad)
                    }
                    .transition(.opacity.combined(with: .scale(scale: 0.95)))
                }
            }
        }
    }
}

// MARK: - NFSU2 Metal Render Yüzeyi (CAMetalLayer + CADisplayLink Köprüsü)

public final class NFSU2MetalRenderUIView: UIView {
    public override class var layerClass: AnyClass {
        CAMetalLayer.self
    }
    
    public var metalLayer: CAMetalLayer {
        layer as! CAMetalLayer
    }
    
    private var displayLink: CADisplayLink?
    // Mouse emulation uses one pointer. Keep its identity stable across UIKit
    // touch batches so a second finger cannot steal movement or release it.
    private var primaryPointerTouch: UITouch?
    private var pointerTouchStartTime: Double = 0.0
    private var pointerTouchToken: UInt64 = 0
#if NFS_FULL_RUNTIME
    private var executablePath: String?
    private var assetRootPath: String?
    private var saveRootPath: String?
    private var sessionStartRequested = false
    private var sessionStartAttempted = false
    private var sessionStatusTimer: Timer?
    private var sessionStatusHandler: ((Int32, String) -> Void)?
#endif
    
    public override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .black
        isOpaque = true
        contentScaleFactor = UIScreen.main.nativeScale
        
        let mtl = metalLayer
        mtl.pixelFormat = .bgra8Unorm
        mtl.framebufferOnly = false
        mtl.contentsGravity = .resizeAspect
        mtl.contentsScale = contentScaleFactor
        
#if NFS_FULL_RUNTIME
        // The strong runtime owns this CAMetalLayer. Attach it on UIKit's
        // thread before the guest creates any renderer resources.
        nfs_game_session_prepare_log()
        metal_init_ios(Unmanaged.passUnretained(mtl).toOpaque())
#else
        metal_init()
        metal_set_layer(Unmanaged.passUnretained(mtl).toOpaque())
#endif
        startDisplayLink()
        setupNotificationObservers()
        setupPointerInteraction()
    }
    
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
    
    private func setupNotificationObservers() {
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(applicationWillResignActive(_:)),
            name: UIApplication.willResignActiveNotification,
            object: nil
        )
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(applicationDidEnterBackground(_:)),
            name: UIApplication.didEnterBackgroundNotification,
            object: nil
        )
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(applicationDidBecomeActive(_:)),
            name: UIApplication.didBecomeActiveNotification,
            object: nil
        )
    }

    @objc private func applicationWillResignActive(_ notification: Notification) {
        stopDisplayLink()
        primaryPointerTouch = nil
        pointerTouchToken &+= 1
        nfsu2_input_clear_all()
        nfsu2_input_set_cursor_available(false)
#if NFS_FULL_RUNTIME
        nfs_game_session_set_active(0)
#endif
    }
    
    @objc private func applicationDidEnterBackground(_ notification: Notification) {
        stopDisplayLink()
        primaryPointerTouch = nil
        pointerTouchToken &+= 1
        nfsu2_input_clear_all()
        nfsu2_input_set_cursor_available(false)
#if NFS_FULL_RUNTIME
        nfs_game_session_set_active(0)
#endif
    }
    
    @objc private func applicationDidBecomeActive(_ notification: Notification) {
        startDisplayLink()
#if NFS_FULL_RUNTIME
        nfs_game_session_set_active(1)
#endif
    }
    
    public override func layoutSubviews() {
        super.layoutSubviews()
        let scale = (window?.screen ?? UIScreen.main).nativeScale
        contentScaleFactor = scale
        metalLayer.contentsScale = scale
        let dw = UInt32((bounds.width * scale).rounded())
        let dh = UInt32((bounds.height * scale).rounded())
        if dw > 0 && dh > 0 {
            metalLayer.drawableSize = CGSize(width: CGFloat(dw), height: CGFloat(dh))
            metal_resize(dw, dh)
            var gw: UInt32 = 1280
            var gh: UInt32 = 720
            metal_game_size(&gw, &gh)
            nfsu2_input_set_viewport(UInt32(bounds.width), UInt32(bounds.height), gw, gh)
#if NFS_FULL_RUNTIME
            startSessionIfReady()
#endif
        }
    }
    
    private func startDisplayLink() {
        guard displayLink == nil else { return }
        let dl = CADisplayLink(target: self, selector: #selector(renderTick))
        dl.preferredFrameRateRange = CAFrameRateRange(minimum: 60, maximum: 120, preferred: 60)
        dl.add(to: .main, forMode: .common)
        displayLink = dl
    }
    
    private func stopDisplayLink() {
        displayLink?.invalidate()
        displayLink = nil
    }
    
    @objc private func renderTick() {
        _ = metal_render_frame()
    }
    
    deinit {
        stopDisplayLink()
#if NFS_FULL_RUNTIME
        sessionStatusTimer?.invalidate()
        // The guest/runtime is process-lifetime state. View teardown only
        // releases input and presentation; it never unmaps live guest memory.
        primaryPointerTouch = nil
        nfsu2_input_clear_all()
        nfs_game_session_set_active(0)
#endif
        NotificationCenter.default.removeObserver(self)
    }

#if NFS_FULL_RUNTIME
    private func publishSessionStatus(_ state: Int32, _ message: String) {
        let handler = sessionStatusHandler
        DispatchQueue.main.async { handler?(state, message) }
    }

    func configureSession(executablePath: String?, assetRootPath: String?,
                          saveRootPath: String?, startRequested: Bool,
                          statusHandler: ((Int32, String) -> Void)?) {
        self.executablePath = executablePath
        self.assetRootPath = assetRootPath
        self.saveRootPath = saveRootPath
        self.sessionStartRequested = startRequested
        self.sessionStatusHandler = statusHandler
        if startRequested { startSessionIfReady() }
    }

    private func startSessionIfReady() {
        guard sessionStartRequested, !sessionStartAttempted,
              bounds.width > 0, bounds.height > 0 else { return }
        guard let executablePath, let assetRootPath, let saveRootPath else {
            sessionStartAttempted = true
            publishSessionStatus(Int32(NFS_GAME_SESSION_FAILED),
                "The verified executable or sandbox roots are unavailable.")
            return
        }
        guard metal_available() != 0, metal_window_ready() != 0 else {
            sessionStartAttempted = true
            publishSessionStatus(Int32(NFS_GAME_SESSION_FAILED),
                "The real Metal device or CAMetalLayer is not ready; the guest was not started.")
            return
        }

        sessionStartAttempted = true
        publishSessionStatus(Int32(NFS_GAME_SESSION_PREPARING),
            "Authenticating SPEED2.EXE and preparing the full runtime…")
        let timer = Timer(timeInterval: 0.75, repeats: true) { [weak self] _ in
            self?.refreshSessionStatus()
        }
        sessionStatusTimer = timer
        RunLoop.main.add(timer, forMode: .common)

        GameFileState.runtimeQueue.async { [weak self] in
            var resultBuffer = [CChar](repeating: 0, count: 512)
            let result = executablePath.withCString { exe in
                assetRootPath.withCString { assets in
                    saveRootPath.withCString { saves in
                        resultBuffer.withUnsafeMutableBufferPointer { buffer in
                            nfs_game_session_start(exe, assets, saves,
                                buffer.baseAddress, buffer.count)
                        }
                    }
                }
            }
            let immediateMessage = String(cString: resultBuffer)
            DispatchQueue.main.async {
                guard let self else { return }
                if result != 0 {
                    self.publishSessionStatus(nfs_game_session_get_state(), immediateMessage)
                }
                self.refreshSessionStatus()
            }
        }
    }

    private func refreshSessionStatus() {
        var buffer = [CChar](repeating: 0, count: 512)
        let state = nfs_game_session_get_state()
        buffer.withUnsafeMutableBufferPointer { ptr in
            nfs_game_session_get_status(ptr.baseAddress, ptr.count)
        }
        let message = String(cString: buffer)
        publishSessionStatus(state, message)
        if state == Int32(NFS_GAME_SESSION_FAILED) || state == Int32(NFS_GAME_SESSION_EXITED) {
            sessionStatusTimer?.invalidate()
            sessionStatusTimer = nil
        }
    }
#endif



    private func setupPointerInteraction() {
        isUserInteractionEnabled = true
        isMultipleTouchEnabled = true
        let hover = UIHoverGestureRecognizer(target: self, action: #selector(handleHover(_:)))
        addGestureRecognizer(hover)
    }

    private func refreshInputViewport() {
#if NFS_FULL_RUNTIME
        guard bounds.width > 0, bounds.height > 0 else { return }
        var gameWidth: UInt32 = 0, gameHeight: UInt32 = 0
        metal_game_size(&gameWidth, &gameHeight)
        nfsu2_input_set_viewport(UInt32(bounds.width), UInt32(bounds.height), gameWidth, gameHeight)
#endif
    }

    @objc private func handleHover(_ recognizer: UIHoverGestureRecognizer) {
        refreshInputViewport()
        let loc = recognizer.location(in: self)
        switch recognizer.state {
        case .began, .changed:
            nfsu2_input_set_cursor(Float(loc.x), Float(loc.y), false)
            nfsu2_input_set_cursor_available(true)
        case .ended, .cancelled:
            nfsu2_input_set_cursor_available(false)
        default:
            break
        }
    }

    public override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesBegan(touches, with: event)
        guard primaryPointerTouch == nil, let touch = touches.first else { return }
        primaryPointerTouch = touch
        pointerTouchStartTime = CACurrentMediaTime()
        pointerTouchToken &+= 1
        refreshInputViewport()
        let loc = touch.location(in: self)
        nfsu2_input_set_cursor(Float(loc.x), Float(loc.y), true)
        nfsu2_input_set_cursor_available(true)
    }

    public override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesMoved(touches, with: event)
        guard let touch = primaryPointerTouch, touches.contains(touch) else { return }
        refreshInputViewport()
        let loc = touch.location(in: self)
        nfsu2_input_set_cursor(Float(loc.x), Float(loc.y), true)
    }

    public override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesEnded(touches, with: event)
        guard let touch = primaryPointerTouch, touches.contains(touch) else { return }
        refreshInputViewport()
        let loc = touch.location(in: self)
        let x = Float(loc.x)
        let y = Float(loc.y)
        let elapsed = CACurrentMediaTime() - pointerTouchStartTime
        let minHoldDuration: Double = 0.050
        let token = pointerTouchToken
        primaryPointerTouch = nil
        if elapsed < minHoldDuration {
            nfsu2_input_set_cursor(x, y, true)
            DispatchQueue.main.asyncAfter(deadline: .now() + (minHoldDuration - elapsed)) { [weak self] in
                guard let self, self.pointerTouchToken == token, self.primaryPointerTouch == nil else { return }
                nfsu2_input_set_cursor(x, y, false)
            }
        } else {
            nfsu2_input_set_cursor(x, y, false)
        }
    }

    public override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesCancelled(touches, with: event)
        guard let touch = primaryPointerTouch, touches.contains(touch) else { return }
        pointerTouchToken &+= 1
        refreshInputViewport()
        let loc = touch.location(in: self)
        nfsu2_input_set_cursor(Float(loc.x), Float(loc.y), false)
        nfsu2_input_set_cursor_available(false)
        primaryPointerTouch = nil
    }
}

public struct NFSU2MetalSurfaceView: UIViewRepresentable {
    private var executablePath: String?
    private var assetRootPath: String?
    private var saveRootPath: String?
    private var startRequested: Bool
    private var statusHandler: ((Int32, String) -> Void)?

    public init(executablePath: String? = nil, assetRootPath: String? = nil,
                saveRootPath: String? = nil, startRequested: Bool = false,
                statusHandler: ((Int32, String) -> Void)? = nil) {
        self.executablePath = executablePath
        self.assetRootPath = assetRootPath
        self.saveRootPath = saveRootPath
        self.startRequested = startRequested
        self.statusHandler = statusHandler
    }
    
    public func makeUIView(context: Context) -> NFSU2MetalRenderUIView {
        let view = NFSU2MetalRenderUIView(frame: .zero)
#if NFS_FULL_RUNTIME
        view.configureSession(executablePath: executablePath,
                              assetRootPath: assetRootPath,
                              saveRootPath: saveRootPath,
                              startRequested: startRequested,
                              statusHandler: statusHandler)
#endif
        return view
    }
    
    public func updateUIView(_ uiView: NFSU2MetalRenderUIView, context: Context) {
#if NFS_FULL_RUNTIME
        uiView.configureSession(executablePath: executablePath,
                                assetRootPath: assetRootPath,
                                saveRootPath: saveRootPath,
                                startRequested: startRequested,
                                statusHandler: statusHandler)
#endif
    }
}

// MARK: - GameMetalView (Metal Renderer + Dokunmatik Kontroller + GameController Entegrasyonu)

public struct GameMetalView: View {
    @Environment(\.scenePhase) private var scenePhase
    @EnvironmentObject private var files: GameFileState
    @StateObject private var controllerManager = GameControllerManager.shared
    @AppStorage("controls.touchMode") private var touchMode = 0
    @AppStorage("controls.useSlider") private var useSlider = true
    @AppStorage("controls.manualGears") private var manualGears = false
    @State private var showSettings = false
    @State private var touchGeneration = UUID()
#if NFS_FULL_RUNTIME
    @State private var startRuntimeRequested = false
    @State private var sessionState = Int32(NFS_GAME_SESSION_IDLE)
    @State private var activeFileImport: GameFileSelectionRequest?
    @State private var sessionStatus = "Game files imported. Start begins one persistent guest session."

    private func completeGameFolderImport(_ request: GameFileSelectionRequest,
                                          outcome: GameFileSelectionOutcome) {
        switch (request, outcome) {
        case (.gameFolder, .selected(let url)):
            files.importGameAssets(from: url)
        case (.gameFolder, .failed(let error)):
            files.assetImportMessage = "Asset folder selection failed: \(error.localizedDescription)"
        case (.executable, .selected), (.executable, .failed):
            files.assetImportMessage = "This screen accepts a game folder. Choose SPEED2.EXE from the app setup screen."
        case (_, .cancelled):
            break
        }
    }

    private var runtimeLogURL: URL? {
        var path = [CChar](repeating: 0, count: 2048)
        path.withUnsafeMutableBufferPointer { nfs_game_session_get_log_path($0.baseAddress, $0.count) }
        let value = String(cString: path)
        return value.isEmpty || !FileManager.default.fileExists(atPath: value)
            ? nil : URL(fileURLWithPath: value)
    }
#endif
    
    private var touchControls: Binding<Bool> {
        Binding(get: { touchMode == 1 || (touchMode == 0 && !controllerManager.isControllerConnected) },
                set: { touchMode = $0 ? 1 : 2 })
    }

    private var gameInputSessionActive: Bool {
#if NFS_FULL_RUNTIME
        startRuntimeRequested && scenePhase == .active
#else
        scenePhase == .active
#endif
    }

    private func openSettings() {
        controllerManager.setMenuOpen(true)
        nfsu2_input_set_enabled(false)
        nfsu2_input_clear_all()
        nfsu2_input_set_cursor_available(false)
        touchGeneration = UUID()
        showSettings = true
    }

    private var settingsPanel: some View {
#if NFS_FULL_RUNTIME
        GameSettingsPanel(touchMode: $touchMode, useSlider: $useSlider, manualGears: $manualGears,
                          runtimeStatus: sessionStatus, runtimeLogURL: runtimeLogURL)
#else
        GameSettingsPanel(touchMode: $touchMode, useSlider: $useSlider, manualGears: $manualGears,
                          runtimeStatus: "Önizleme", runtimeLogURL: nil)
#endif
    }
    
    public init() {}
    
    public var body: some View {
        ZStack {
#if NFS_FULL_RUNTIME
            NFSU2MetalSurfaceView(
                executablePath: files.executablePath,
                assetRootPath: files.assetRootPath,
                saveRootPath: files.saveRootPath,
                startRequested: startRuntimeRequested,
                statusHandler: { state, message in
                    sessionState = state
                    sessionStatus = message
                })
                .ignoresSafeArea()

            if startRuntimeRequested {
                if !showSettings {
                    OnScreenControlsOverlay(showControls: touchControls, openSettings: openSettings)
                        .id(touchGeneration)
                }
                if sessionState == Int32(NFS_GAME_SESSION_FAILED) {
                    VStack(spacing: 12) {
                        Text("Oyun durdu").font(.headline)
                        Text(sessionStatus).font(.footnote).textSelection(.enabled)
                            .accessibilityIdentifier("full-runtime-failure")
                        Button("Ayarlar ve günlük") { openSettings() }
                    }
                    .padding().background(.regularMaterial, in: RoundedRectangle(cornerRadius: 16))
                }
            } else {
                VStack(spacing: 12) {
                    Text("FULL RUNTIME · GAME FILES IMPORTED")
                        .font(.system(.headline, design: .monospaced, weight: .bold))
                        .foregroundStyle(.white)
                    Text("The saved executable is verified and game files are staged in the app sandbox. Asset completeness is not validated. Saves use a separate sandbox directory.")
                        .font(.system(.footnote, design: .monospaced))
                        .foregroundStyle(.white.opacity(0.75))
                        .multilineTextAlignment(.center)
                        .frame(maxWidth: 520)
                    Button {
                        startRuntimeRequested = true
                        nfsu2_input_clear_all()
                        nfsu2_input_set_enabled(scenePhase == .active)
                        controllerManager.setActive(scenePhase == .active)
                    } label: {
                        Label("START FULL RUNTIME", systemImage: "play.fill")
                            .fontWeight(.bold)
                            .padding(.horizontal, 18)
                            .padding(.vertical, 12)
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.mint)
                    .disabled(files.assetImportRunning)
                    .accessibilityIdentifier("full-runtime-start")
                    Button {
                        activeFileImport = .gameFolder
                    } label: {
                        Label(files.assetImportRunning ? "IMPORTING…" : "IMPORT / RETRY GAME FOLDER", systemImage: "folder.badge.plus")
                    }
                    .disabled(files.assetImportRunning || activeFileImport != nil)
                    .accessibilityIdentifier("select-game-folder")
                    .accessibilityHint("Opens Files so you can select the full game folder.")
                    Text("You can also copy the full game folder into Files › On My iPad › NFSU2 iOS › Game Import.")
                        .font(.system(.caption2, design: .monospaced))
                        .foregroundStyle(.white.opacity(0.68))
                        .multilineTextAlignment(.center)
                        .frame(maxWidth: 520)
                    Button {
                        files.refreshSharedImport(force: true)
                    } label: {
                        Label(files.assetImportRunning ? "CHECKING FILES FOLDER…" : "CHECK FILES FOLDER",
                              systemImage: files.assetImportRunning ? "hourglass" : "folder")
                    }
                    .buttonStyle(.bordered)
                    .disabled(files.assetImportRunning || activeFileImport != nil)
                    .accessibilityIdentifier("check-game-files-folder")
                    .accessibilityHint("Checks Files, On My iPad, NFSU2 iOS, Game Import for the verified game folder.")
                    Text(files.assetImportMessage)
                        .font(.caption)
                        .foregroundStyle(.white.opacity(0.7))
                    if let url = runtimeLogURL {
                        ShareLink(item: url) { Label("Share runtime log", systemImage: "square.and.arrow.up") }
                    }
                    Text("Starting the guest does not confirm a visible game frame or playable race.")
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(.white.opacity(0.65))
                        .multilineTextAlignment(.center)
                        .frame(maxWidth: 520)
                }
                .padding(20)
                .background(.black.opacity(0.72), in: RoundedRectangle(cornerRadius: 16))
                .padding(24)
            }
#else
            NFSU2MetalSurfaceView()
                .ignoresSafeArea()

            if !showSettings {
                OnScreenControlsOverlay(showControls: touchControls, openSettings: openSettings)
                    .id(touchGeneration)
            }
#endif
        }
        .allowsHitTesting(!showSettings)
        .sheet(isPresented: $showSettings, onDismiss: {
            nfsu2_input_clear_all()
            nfsu2_input_set_enabled(gameInputSessionActive)
            controllerManager.setMenuOpen(false)
            touchGeneration = UUID()
        }) { settingsPanel }
#if NFS_FULL_RUNTIME
        .gameFileImporter(selection: $activeFileImport) { request, outcome in
            completeGameFolderImport(request, outcome: outcome)
        }
#endif
        .onAppear {
            nfsu2_input_set_enabled(gameInputSessionActive && !showSettings)
            controllerManager.checkForExistingControllers()
            controllerManager.setActive(gameInputSessionActive)
#if NFS_FULL_RUNTIME
            nfs_game_session_set_active(1)
#endif
        }
        .onDisappear {
            nfsu2_input_set_enabled(false)
            controllerManager.setActive(false)
            nfsu2_input_clear_all()
#if NFS_FULL_RUNTIME
            nfs_game_session_set_active(0)
#endif
        }
        .onChange(of: scenePhase) { phase in
            nfsu2_input_set_enabled(gameInputSessionActive && !showSettings)
            controllerManager.setActive(gameInputSessionActive)
            touchGeneration = UUID()
            if phase != .active {
                // Uygulama arka plana geçtiğinde veya kesintiye uğradığında takılı tuşları sıfırla
                nfsu2_input_clear_all()
#if NFS_FULL_RUNTIME
                nfsu2_input_set_cursor_available(false)
                nfs_game_session_set_active(0)
#endif
            } else {
#if NFS_FULL_RUNTIME
                nfs_game_session_set_active(1)
#endif
            }
        }
        .onChange(of: touchMode) { _ in
            nfsu2_input_clear_all()
            touchGeneration = UUID()
        }
        .onChange(of: controllerManager.isControllerConnected) { _ in
            nfsu2_input_clear_all()
            touchGeneration = UUID()
        }
    }
}
