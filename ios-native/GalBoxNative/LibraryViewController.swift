import UIKit
import UniformTypeIdentifiers

private struct PackageItem {
    let title: String
    let path: String
    let engine: String
    let detail: String
}

final class LibraryViewController: UIViewController, UITableViewDataSource,
                                   UITableViewDelegate, UIDocumentPickerDelegate {

    private let table = UITableView(frame: .zero, style: .insetGrouped)
    private var items: [PackageItem] = []

    private static let engineNames: [String: String] = [
        "kirikiri": "Kirikiri / XP3",
        "onscripter": "ONScripter / NSA",
        "renpy": "Ren'Py",
        "kag-script": "KAG 剧本",
        "unknown": "未识别"
    ]

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "GalBox 原生"
        view.backgroundColor = UIColor(red: 0.06, green: 0.07, blue: 0.11, alpha: 1)

        navigationItem.rightBarButtonItem = UIBarButtonItem(
            barButtonSystemItem: .add, target: self, action: #selector(importPackage))

        let header = UILabel()
        header.text = "iOS 多引擎 Galgame 播放器 · 原生 C++ 内核\n导入 .xp3 游戏包，按特征文件判定引擎后启动"
        header.numberOfLines = 0
        header.textColor = .secondaryLabel
        header.font = .systemFont(ofSize: 13)
        header.textAlignment = .center
        header.frame = CGRect(x: 0, y: 0, width: 300, height: 56)
        table.tableHeaderView = header

        table.register(UITableViewCell.self, forCellReuseIdentifier: "cell")
        table.dataSource = self
        table.delegate = self
        table.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(table)
        NSLayoutConstraint.activate([
            table.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            table.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            table.topAnchor.constraint(equalTo: view.topAnchor),
            table.bottomAnchor.constraint(equalTo: view.bottomAnchor)
        ])
        reload()
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        reload()
    }

    private func documentsURL() -> URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    }

    private func reload() {
        var found: [PackageItem] = []

        // Bundled sample package so the app is playable immediately after install.
        if let demo = Bundle.main.path(forResource: "demo-game", ofType: "xp3") {
            found.append(PackageItem(title: "内置演示 · 星屑来信",
                                     path: demo, engine: "kirikiri",
                                     detail: "随 App 附带的合法 XP3 夹具，含 .ks 剧本与压缩图片"))
        }

        let fm = FileManager.default
        let root = documentsURL()
        let names = (try? fm.contentsOfDirectory(atPath: root.path)) ?? []
        for name in names.sorted() {
            let path = root.appendingPathComponent(name).path
            var isDir: ObjCBool = false
            guard fm.fileExists(atPath: path, isDirectory: &isDir) else { continue }
            if isDir.boolValue {
                if name.hasSuffix(".app") || name == "saves" { continue }
                let engine = GBEngineBridge.detectEngine(atPath: path)
                let count = (try? fm.contentsOfDirectory(atPath: path).count) ?? 0
                found.append(PackageItem(title: name, path: path, engine: engine,
                                         detail: "文件夹 · \(count) 个条目"))
            } else if name.lowercased().hasSuffix(".xp3") {
                let attrs = try? fm.attributesOfItem(atPath: path)
                let bytes = (attrs?[.size] as? NSNumber)?.int64Value ?? 0
                found.append(PackageItem(title: name, path: path, engine: "kirikiri",
                                         detail: String(format: "XP3 归档 · %.1f MB", Double(bytes) / 1048576.0)))
            }
        }
        items = found
        table.reloadData()
    }

    @objc private func importPackage() {
        let picker = UIDocumentPickerViewController(forOpeningContentTypes: [.item, .folder])
        picker.allowsMultipleSelection = true
        picker.delegate = self
        present(picker, animated: true)
    }

    func documentPicker(_ controller: UIDocumentPickerViewController,
                        didPickDocumentsAt urls: [URL]) {
        let fm = FileManager.default
        for url in urls {
            guard url.startAccessingSecurityScopedResource() else { continue }
            defer { url.stopAccessingSecurityScopedResource() }
            let dest = documentsURL().appendingPathComponent(url.lastPathComponent)
            try? fm.removeItem(at: dest)
            do {
                try fm.copyItem(at: url, to: dest)
            } catch {
                showMessage("导入失败", error.localizedDescription)
            }
        }
        reload()
    }

    private func showMessage(_ title: String, _ msg: String) {
        let a = UIAlertController(title: title, message: msg, preferredStyle: .alert)
        a.addAction(UIAlertAction(title: "好", style: .default))
        present(a, animated: true)
    }

    // MARK: - table

    func numberOfSections(in tableView: UITableView) -> Int { 1 }

    func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        items.count
    }

    func tableView(_ tableView: UITableView,
                   cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: "cell", for: indexPath)
        let item = items[indexPath.row]
        var cfg = cell.defaultContentConfiguration()
        cfg.text = item.title
        cfg.secondaryText = (Self.engineNames[item.engine] ?? item.engine) + " · " + item.detail
        cfg.image = UIImage(systemName: item.engine == "kirikiri" ? "gamecontroller.fill" : "folder.fill")
        cfg.imageProperties.tintColor = UIColor(red: 0.45, green: 0.74, blue: 1.0, alpha: 1)
        cell.contentConfiguration = cfg
        cell.accessoryType = .disclosureIndicator
        cell.backgroundColor = UIColor(red: 0.11, green: 0.13, blue: 0.19, alpha: 1)
        return cell
    }

    func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        let item = items[indexPath.row]
        let vc = GameViewController(packagePath: item.path, title: item.title)
        navigationController?.pushViewController(vc, animated: true)
    }
}
