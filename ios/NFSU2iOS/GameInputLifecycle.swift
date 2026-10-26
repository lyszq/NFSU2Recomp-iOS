// The game view owns session/settings intent; UIKit owns application activation.
// Neither a captured SwiftUI scenePhase nor a renderer resume may overwrite intent.
struct GameInputLifecycle {
    var sessionActive = false
    var applicationActive = false
    var settingsOpen = false

    var acceptsInput: Bool {
        sessionActive && applicationActive && !settingsOpen
    }

    func applyToBridge() {
        // Flush held controls on either transition before accepting fresh input.
        nfsu2_input_clear_all()
        nfsu2_input_set_enabled(acceptsInput)
    }
}
