// Exercises the production Swift policy against the real C input bridge.
@main
struct InputLifecycleFixture {
    static func expect(_ condition: Bool, _ message: String) {
        if !condition { fatalError(message) }
    }

    static func pressBothSources() {
        nfsu2_input_set_key(UInt8(NFSU2_DIK_UP), true)
        nfsu2_input_set_controller_key(UInt8(NFSU2_DIK_LEFT), true)
    }

    static func main() {
        var state = GameInputLifecycle()
        state.applicationActive = true
        state.applyToBridge()
        expect(!nfsu2_input_is_enabled(), "Setup must not accept gameplay input")
        state.sessionActive = true
        state.applyToBridge()
        for _ in 0..<50 {
            pressBothSources()
            expect(nfsu2_input_active_count() == 2, "Both input sources must work")
            state.applicationActive = false // resign active / Home
            state.applyToBridge()
            state.applyToBridge() // duplicate background callback
            pressBothSources()
            expect(!nfsu2_input_is_enabled() && nfsu2_input_active_count() == 0,
                   "Background presses must be discarded")
            state.applicationActive = true // UIKit foreground resume
            state.applyToBridge()
            expect(nfsu2_input_is_enabled() && nfsu2_input_active_count() == 0,
                   "Resume must reopen the common gate without stale keys")
            pressBothSources()
            expect(nfsu2_input_get_key(UInt8(NFSU2_DIK_UP)), "Touch must recover")
            expect(nfsu2_input_get_key(UInt8(NFSU2_DIK_LEFT)), "Controller must recover")
        }
        print("PASS: 50 Home/foreground cycles restore touch and controller on the real bridge")

        state.settingsOpen = true
        state.applyToBridge()
        state.applicationActive = false
        state.applyToBridge()
        state.applicationActive = true
        state.applyToBridge()
        pressBothSources()
        expect(!nfsu2_input_is_enabled() && nfsu2_input_active_count() == 0,
               "Foreground cannot bypass open settings")
        state.settingsOpen = false
        state.applyToBridge()
        pressBothSources()
        expect(nfsu2_input_active_count() == 2, "Settings dismissal must restore both sources")
        print("PASS: foreground preserves the settings gate; dismiss restores controls")

        state.sessionActive = false // real view disappearance / setup
        state.applyToBridge()
        state.applicationActive = false
        state.applyToBridge()
        state.applicationActive = true
        state.applyToBridge()
        expect(!nfsu2_input_is_enabled(), "Foreground cannot enable an absent game session")
        print("PASS: background/foreground does not override session visibility")
    }
}
