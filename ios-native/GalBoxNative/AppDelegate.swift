import UIKit

@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
    var window: UIWindow?

    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        // Galgame reading is a long idle activity: keep the screen awake.
        application.isIdleTimerDisabled = true

        let window = UIWindow(frame: UIScreen.main.bounds)
        let nav = UINavigationController(rootViewController: LibraryViewController())
        nav.navigationBar.prefersLargeTitles = true
        nav.navigationBar.tintColor = UIColor(red: 0.45, green: 0.74, blue: 1.0, alpha: 1)
        window.rootViewController = nav
        window.makeKeyAndVisible()
        self.window = window
        return true
    }
}
