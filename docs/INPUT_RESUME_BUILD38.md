# Foreground input recovery - v0.27.3 / build 38

The user confirms build 37's gameplay DS4 rumble now works. They report that
Home followed by returning to the running app leaves both touch and DS4 controls
unresponsive, while game rendering/audio continue.

The render UIView's UIKit activation callback resumed the guest and display link,
but did not reopen the common input bridge or activate/rebind the controller.
Input restoration depended separately on SwiftUI scenePhase callbacks and their
captured view/session state. Both layers could therefore disagree about activity.
No device log established the precise callback ordering of the reported failure.

The controller manager now owns the common input gate through GameInputLifecycle.
The game view supplies session/settings intent; the render UIView supplies UIKit
activation. Foreground restoration applies the same policy to the C bridge, clears
interrupted keys and rebinds controller handlers. SwiftUI scenePhase only rebuilds
touch gestures; it no longer writes the common gate or controller activity.
Settings and absent-session gates remain effective during foreground restoration.
Held controller keys must return to neutral before being accepted, as before.
Build 37 physics rumble, game memory, assets and saves are retained.

Validation prepared locally:

- 261 Python contracts passed.
- 16 C bridge tests passed, including 50 disable/resume cycles using both touch
  and controller sources, and the existing concurrent rumble regression.
- Added an Apple-host executable that runs the production Swift policy against
  the real C bridge for 50 Home/resume cycles, settings-open resume, and an absent
  game session. Windows does not have Swift/UIKit; Apple CI is required to run it
  and typecheck the complete production UI.

Device acceptance: during a race, use Home and return repeatedly. New touch and
DS4 presses must work; interrupted keys must not remain held. Repeat with Settings
open, dismiss Settings, and check that gameplay input and existing rumble recover.
This is not yet physical-device verification of build 38.
