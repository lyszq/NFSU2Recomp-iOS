import XCTest

/// The private runner stages the real authenticated EXE and an explicit
/// missing-assets sentinel. This tests the actual Start/lifecycle controls;
/// it does not assert that the game boots, renders a scene or plays sound.
final class FullRuntimeSmokeTests: XCTestCase {
    func testFullRuntimeStartsAndUIKitRemainsResponsiveWithoutGameAssets() throws {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launch()
        XCTAssertTrue(app.wait(for: .runningForeground, timeout: 20))

        let start = app.buttons["full-runtime-start"]
        XCTAssertTrue(start.waitForExistence(timeout: 20), "Authenticated full-runtime Start UI must be visible")
        start.tap()

        // Runtime details now live in the settings sheet.
        let settings = app.buttons["game-settings"]
        XCTAssertTrue(settings.waitForExistence(timeout: 20))
        settings.tap()

        let status = app.staticTexts["full-runtime-status"].firstMatch
        XCTAssertTrue(status.waitForExistence(timeout: 20))
        let initialized = NSPredicate { _, _ in
            let message = status.label
            return ["Full runtime initialized", "Full runtime active",
                    "Guest process exited", "Primary guest thread exited",
                    "Guest entry returned"].contains { message.hasPrefix($0) }
        }
        expectation(for: initialized, evaluatedWith: status)
        waitForExpectations(timeout: 30)
        XCTAssertFalse(status.label.contains("guest was not started"), status.label)

        // Exercise real scene inactivity/resume, then query accessibility again.
        // A persistent guest loop may continue without assets; the runner kills
        // this disposable simulator process after collecting its private log.
        XCUIDevice.shared.press(.home)
        app.activate()
        XCTAssertTrue(app.wait(for: .runningForeground, timeout: 20))
        XCTAssertTrue(status.waitForExistence(timeout: 10))
        let screen = XCTAttachment(screenshot: app.screenshot())
        screen.name = "Full runtime missing-assets smoke - scene unverified"
        screen.lifetime = .keepAlways
        add(screen)
        let evidence = XCTAttachment(string: status.label)
        evidence.name = "Native session status after actual Start and foreground resume"
        evidence.lifetime = .keepAlways
        add(evidence)
    }
}
