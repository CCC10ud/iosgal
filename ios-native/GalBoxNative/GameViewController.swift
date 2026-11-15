import UIKit

/// Pure UIKit renderer backed by the C++ engine; no web runtime of any kind.
final class GameViewController: UIViewController {

    private let packagePath: String
    private let gameTitle: String

    private let bgView = UIImageView()
    private let fgView = UIImageView()
    private let clockLabel = UILabel()
    private let nameLabel = UILabel()
    private let textLabel = UILabel()
    private let textBox = UIView()
    private let choiceStack = UIStackView()
    private let toolBar = UIStackView()
    private let endPanel = UIView()
    private let endLabel = UILabel()
    private let toastLabel = UILabel()

    private var step: [String: Any] = [:]
    private var fullText = ""
    private var shown = 0
    private var typeTimer: Timer?
    private var autoTimer: Timer?
    private var skipTimer: Timer?
    private var autoOn = false
    private var skipOn = false

    init(packagePath: String, title: String) {
        self.packagePath = packagePath
        self.gameTitle = title
        super.init(nibName: nil, bundle: nil)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = UIColor(red: 0.04, green: 0.05, blue: 0.09, alpha: 1)
        navigationController?.setNavigationBarHidden(true, animated: false)
        buildUI()
        openPackage()
    }

    override func viewWillDisappear(_ animated: Bool) {
        super.viewWillDisappear(animated)
        stopAllTimers()
        navigationController?.setNavigationBarHidden(false, animated: animated)
    }

    // MARK: - UI

    private func buildUI() {
        bgView.contentMode = .scaleAspectFill
        bgView.clipsToBounds = true
        bgView.backgroundColor = UIColor(red: 0.10, green: 0.14, blue: 0.28, alpha: 1)
        bgView.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(bgView)

        fgView.contentMode = .scaleAspectFit
        fgView.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(fgView)

        let topBar = UIView()
        topBar.backgroundColor = UIColor.black.withAlphaComponent(0.28)
        topBar.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(topBar)

        clockLabel.text = "9:41   ●●● 100%"
        clockLabel.textColor = .white
        clockLabel.font = .monospacedDigitSystemFont(ofSize: 13, weight: .semibold)
        clockLabel.textAlignment = .center
        clockLabel.translatesAutoresizingMaskIntoConstraints = false
        topBar.addSubview(clockLabel)

        textBox.backgroundColor = UIColor.black.withAlphaComponent(0.62)
        textBox.layer.cornerRadius = 16
        textBox.layer.borderWidth = 1
        textBox.layer.borderColor = UIColor.white.withAlphaComponent(0.18).cgColor
        textBox.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(textBox)

        nameLabel.textColor = UIColor(red: 0.56, green: 0.82, blue: 1.0, alpha: 1)
        nameLabel.font = .boldSystemFont(ofSize: 16)
        nameLabel.translatesAutoresizingMaskIntoConstraints = false
        textBox.addSubview(nameLabel)

        textLabel.textColor = .white
        textLabel.font = .systemFont(ofSize: 17)
        textLabel.numberOfLines = 0
        textLabel.translatesAutoresizingMaskIntoConstraints = false
        textBox.addSubview(textLabel)

        choiceStack.axis = .vertical
        choiceStack.spacing = 10
        choiceStack.alignment = .fill
        choiceStack.distribution = .fillEqually
        choiceStack.translatesAutoresizingMaskIntoConstraints = false
        choiceStack.isHidden = true
        view.addSubview(choiceStack)

        toolBar.axis = .horizontal
        toolBar.distribution = .fillEqually
        toolBar.spacing = 6
        toolBar.translatesAutoresizingMaskIntoConstraints = false
        for (title, sel) in [("返回", #selector(onBack)), ("存档", #selector(onSave)),
                             ("读档", #selector(onLoad)), ("历史", #selector(onHistory)),
                             ("AUTO", #selector(onAuto)), ("SKIP", #selector(onSkip))] {
            let b = UIButton(type: .system)
            b.setTitle(title, for: .normal)
            b.titleLabel?.font = .systemFont(ofSize: 12, weight: .semibold)
            b.setTitleColor(.white, for: .normal)
            b.backgroundColor = UIColor.white.withAlphaComponent(0.14)
            b.layer.cornerRadius = 8
            b.addTarget(self, action: sel, for: .touchUpInside)
            toolBar.addArrangedSubview(b)
        }
        view.addSubview(toolBar)

        endPanel.backgroundColor = UIColor(red: 0.08, green: 0.09, blue: 0.15, alpha: 0.95)
        endPanel.layer.cornerRadius = 18
        endPanel.isHidden = true
        endPanel.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(endPanel)
        endLabel.textColor = .white
        endLabel.textAlignment = .center
        endLabel.numberOfLines = 0
        endLabel.font = .systemFont(ofSize: 18, weight: .semibold)
        endLabel.translatesAutoresizingMaskIntoConstraints = false
        endPanel.addSubview(endLabel)
        let again = UIButton(type: .system)
        again.setTitle("返回游戏库", for: .normal)
        again.addTarget(self, action: #selector(onBack), for: .touchUpInside)
        again.translatesAutoresizingMaskIntoConstraints = false
        endPanel.addSubview(again)

        toastLabel.textColor = .white
        toastLabel.backgroundColor = UIColor.black.withAlphaComponent(0.75)
        toastLabel.layer.cornerRadius = 10
        toastLabel.clipsToBounds = true
        toastLabel.textAlignment = .center
        toastLabel.font = .systemFont(ofSize: 13)
        toastLabel.alpha = 0
        toastLabel.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(toastLabel)

        let safe = view.safeAreaLayoutGuide
        NSLayoutConstraint.activate([
            bgView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            bgView.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            bgView.topAnchor.constraint(equalTo: view.topAnchor),
            bgView.bottomAnchor.constraint(equalTo: view.bottomAnchor),

            fgView.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            fgView.widthAnchor.constraint(equalTo: view.widthAnchor, multiplier: 0.78),
            fgView.heightAnchor.constraint(equalTo: view.heightAnchor, multiplier: 0.58),
            fgView.bottomAnchor.constraint(equalTo: textBox.topAnchor, constant: -6),

            topBar.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            topBar.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            topBar.topAnchor.constraint(equalTo: view.topAnchor),
            topBar.bottomAnchor.constraint(equalTo: safe.topAnchor, constant: 26),
            clockLabel.leadingAnchor.constraint(equalTo: topBar.leadingAnchor),
            clockLabel.trailingAnchor.constraint(equalTo: topBar.trailingAnchor),
            clockLabel.bottomAnchor.constraint(equalTo: topBar.bottomAnchor, constant: -4),

            toolBar.leadingAnchor.constraint(equalTo: safe.leadingAnchor, constant: 10),
            toolBar.trailingAnchor.constraint(equalTo: safe.trailingAnchor, constant: -10),
            toolBar.bottomAnchor.constraint(equalTo: safe.bottomAnchor, constant: -6),
            toolBar.heightAnchor.constraint(equalToConstant: 38),

            textBox.leadingAnchor.constraint(equalTo: safe.leadingAnchor, constant: 12),
            textBox.trailingAnchor.constraint(equalTo: safe.trailingAnchor, constant: -12),
            textBox.bottomAnchor.constraint(equalTo: toolBar.topAnchor, constant: -10),
            textBox.heightAnchor.constraint(equalToConstant: 148),

            nameLabel.leadingAnchor.constraint(equalTo: textBox.leadingAnchor, constant: 18),
            nameLabel.topAnchor.constraint(equalTo: textBox.topAnchor, constant: 12),
            nameLabel.trailingAnchor.constraint(equalTo: textBox.trailingAnchor, constant: -18),
            textLabel.leadingAnchor.constraint(equalTo: textBox.leadingAnchor, constant: 18),
            textLabel.trailingAnchor.constraint(equalTo: textBox.trailingAnchor, constant: -18),
            textLabel.topAnchor.constraint(equalTo: nameLabel.bottomAnchor, constant: 8),
            textLabel.bottomAnchor.constraint(lessThanOrEqualTo: textBox.bottomAnchor, constant: -12),

            choiceStack.leadingAnchor.constraint(equalTo: safe.leadingAnchor, constant: 28),
            choiceStack.trailingAnchor.constraint(equalTo: safe.trailingAnchor, constant: -28),
            choiceStack.bottomAnchor.constraint(equalTo: textBox.topAnchor, constant: -14),

            endPanel.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            endPanel.centerYAnchor.constraint(equalTo: view.centerYAnchor),
            endPanel.widthAnchor.constraint(equalToConstant: 280),
            endPanel.heightAnchor.constraint(equalToConstant: 170),
            endLabel.leadingAnchor.constraint(equalTo: endPanel.leadingAnchor, constant: 18),
            endLabel.trailingAnchor.constraint(equalTo: endPanel.trailingAnchor, constant: -18),
            endLabel.topAnchor.constraint(equalTo: endPanel.topAnchor, constant: 28),
            again.centerXAnchor.constraint(equalTo: endPanel.centerXAnchor),
            again.bottomAnchor.constraint(equalTo: endPanel.bottomAnchor, constant: -18),

            toastLabel.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            toastLabel.bottomAnchor.constraint(equalTo: toolBar.topAnchor, constant: -16),
            toastLabel.widthAnchor.constraint(greaterThanOrEqualToConstant: 160),
            toastLabel.heightAnchor.constraint(equalToConstant: 30)
        ])

        view.addGestureRecognizer(UITapGestureRecognizer(target: self, action: #selector(onTap)))
    }

    // MARK: - package / script

    private func openPackage() {
        guard GBEngineBridge.openPackage(atPath: packagePath) else {
            showError("无法打开游戏包：\(GBEngineBridge.lastError())")
            return
        }
        let files = GBEngineBridge.fileList()
        let isTJS2 = GBEngineBridge.hasTJS2Startup()
        toast("包内 \(files.count) 个文件 · Kirikiri XP3")

        // M1c：真跑一次 vendored TJS2，把结果直接显示在界面上。
        // 完整字典进设备日志，toast 只放关键指标（label 是单行定高的）。
        let smoke = GBEngineBridge.tjs2SmokeTest()
        NSLog("[GalBox][TJS2] %@", smoke)
        let smokeOK = (smoke["ok"] as? Bool) ?? false
        let diag: String
        if smokeOK {
            let ver = smoke["version"] as? String ?? "?"
            let evalValue = smoke["eval"] as? Int ?? -1
            let rd = smoke["hostReadDelta"] as? Int ?? 0
            let wr = smoke["hostWriteDelta"] as? Int ?? 0
            diag = "TJS2 \(ver) ✓ eval=\(evalValue) · 宿主读\(rd)/写\(wr)"
                 + (isTJS2 ? " · startup.tjs 待 M2" : "")
        } else {
            diag = "TJS2 未就绪：" + ((smoke["reason"] as? String) ?? "未知原因")
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.9) { [weak self] in
            self?.toast(diag)
        }

        var scriptName: String?
        for f in files {
            let n = f["name"] as? String ?? ""
            if n.hasSuffix(".ks") && (n.contains("first") || n.contains("main") || n.contains("start")) {
                scriptName = n
                break
            }
        }
        if scriptName == nil {
            for f in files {
                let n = f["name"] as? String ?? ""
                if n.hasSuffix(".ks") { scriptName = n; break }
            }
        }
        guard let name = scriptName, let data = GBEngineBridge.data(forFile: name),
              let text = String(data: data, encoding: .utf8) else {
            if isTJS2 {
                showError("检测到 TJS2 启动脚本（startup.tjs）。TJS2 引擎已可启动，但 KAG 层（Milestone 2）尚未移植，暂不能执行 startup.tjs；包内也没有可直接播放的 .ks 剧本。")
            } else {
                showError("包内没有可运行的 .ks 剧本")
            }
            return
        }
        guard GBEngineBridge.loadScript(text) else {
            showError("剧本解析失败：\(GBEngineBridge.lastError())")
            return
        }
        render(GBEngineBridge.currentStep())
        // isTJS2 的提示已并入上方 TJS2 冒烟 toast，避免连续两条互相覆盖。
    }

    private func render(_ raw: [String: Any]) {
        step = raw
        let type = raw["type"] as? String ?? "Error"
        endPanel.isHidden = true

        if (raw["clearPage"] as? Bool) == true { textLabel.text = "" }
        if let bg = raw["bg"] as? String, !bg.isEmpty { applyBackground(bg) }
        if let img = raw["image"] as? String, !img.isEmpty { applyCharacter(img) }

        if type == "Error" {
            showError(raw["message"] as? String ?? "未知引擎错误")
            return
        }
        nameLabel.text = (type == "Narr") ? "旁白" : (raw["who"] as? String ?? "")
        nameLabel.isHidden = (type == "End")
        let text = raw["text"] as? String ?? ""
        startTyping(text)

        let opts = raw["options"] as? [String] ?? []
        if type == "Choice" && !opts.isEmpty { showChoices(opts) } else { hideChoices() }
        if type == "End" {
            endLabel.text = "— 剧情结束 —\n" + (text.isEmpty ? gameTitle : text)
            endPanel.isHidden = false
            stopAllTimers()
        }
    }

    private func applyBackground(_ name: String) {
        if let d = GBEngineBridge.data(forFile: name), let img = UIImage(data: d) {
            bgView.image = img
        } else {
            // Sample packages carry placeholder bytes; keep a visible, stable
            // colour instead of a blank screen when a real PNG is unavailable.
            bgView.image = nil
            bgView.backgroundColor = colorFor(name)
        }
    }

    private func applyCharacter(_ name: String) {
        if let d = GBEngineBridge.data(forFile: name), let img = UIImage(data: d) {
            fgView.image = img
        } else {
            fgView.image = UIImage(systemName: "person.fill")?
                .withTintColor(.white.withAlphaComponent(0.35), renderingMode: .alwaysOriginal)
        }
    }

    private func colorFor(_ seed: String) -> UIColor {
        var h: UInt64 = 0
        for u in seed.unicodeScalars { h = h &* 31 &+ UInt64(u.value) }
        let hue = CGFloat(h % 360) / 360.0
        return UIColor(hue: hue, saturation: 0.42, brightness: 0.28, alpha: 1)
    }

    // MARK: - choices / typing / progression

    private func showChoices(_ opts: [String]) {
        for v in choiceStack.arrangedSubviews {
            choiceStack.removeArrangedSubview(v)
            v.removeFromSuperview()
        }
        for (i, title) in opts.enumerated() {
            let b = UIButton(type: .system)
            b.setTitle(title, for: .normal)
            b.tag = i
            b.titleLabel?.font = .systemFont(ofSize: 16, weight: .semibold)
            b.setTitleColor(.white, for: .normal)
            b.backgroundColor = UIColor(red: 0.20, green: 0.36, blue: 0.62, alpha: 0.92)
            b.layer.cornerRadius = 12
            b.heightAnchor.constraint(equalToConstant: 46).isActive = true
            b.addTarget(self, action: #selector(onChoice(_:)), for: .touchUpInside)
            choiceStack.addArrangedSubview(b)
        }
        choiceStack.isHidden = false
    }

    private func hideChoices() { choiceStack.isHidden = true }

    @objc private func onChoice(_ sender: UIButton) {
        finishTyping()
        render(GBEngineBridge.choose(sender.tag))
    }

    private func startTyping(_ text: String) {
        typeTimer?.invalidate()
        fullText = text
        shown = 0
        textLabel.text = ""
        let speed = UserDefaults.standard.double(forKey: "GalBoxTypeSpeed")
        let interval = speed > 0 ? speed : 0.028
        typeTimer = Timer.scheduledTimer(withTimeInterval: interval, repeats: true) { [weak self] t in
            guard let self = self else { t.invalidate(); return }
            self.shown += 1
            let n = min(self.shown, self.fullText.count)
            let idx = self.fullText.index(self.fullText.startIndex, offsetBy: n)
            self.textLabel.text = String(self.fullText[..<idx])
            if n >= self.fullText.count {
                t.invalidate()
                self.typeTimer = nil
            }
        }
    }

    private func finishTyping() {
        typeTimer?.invalidate()
        typeTimer = nil
        textLabel.text = fullText
    }

    @objc private func onTap() {
        if typeTimer != nil { finishTyping(); return }
        advanceOne()
    }

    private func advanceOne() {
        let t = step["type"] as? String ?? ""
        if t == "Choice" || t == "End" || t == "Error" { return }
        render(GBEngineBridge.advance())
    }

    // MARK: - toolbar

    @objc private func onBack() {
        GBEngineBridge.closePackage()
        navigationController?.popViewController(animated: true)
    }

    @objc private func onSave() {
        let a = UIAlertController(title: "存档", message: "选择槽位", preferredStyle: .actionSheet)
        for i in 0..<3 {
            a.addAction(UIAlertAction(title: "槽位 \(i + 1)", style: .default) { [weak self] _ in
                self?.saveSlot(i)
            })
        }
        a.addAction(UIAlertAction(title: "取消", style: .cancel))
        present(a, animated: true)
    }

    @objc private func onLoad() {
        let a = UIAlertController(title: "读档", message: "选择槽位", preferredStyle: .actionSheet)
        for i in 0..<3 {
            a.addAction(UIAlertAction(title: "槽位 \(i + 1)", style: .default) { [weak self] _ in
                self?.loadSlot(i)
            })
        }
        a.addAction(UIAlertAction(title: "取消", style: .cancel))
        present(a, animated: true)
    }

    @objc private func onHistory() {
        let h = GBEngineBridge.history()
        if h.isEmpty { toast("还没有历史记录"); return }
        let a = UIAlertController(title: "历史回放", message: nil, preferredStyle: .actionSheet)
        for (i, item) in h.suffix(12).enumerated() {
            let who = item["who"] as? String ?? ""
            let text = item["text"] as? String ?? ""
            let label = (who.isEmpty ? "" : who + "：") + text
            a.addAction(UIAlertAction(title: String(label.prefix(40)), style: .default) { [weak self] _ in
                guard let self = self else { return }
                let realIndex = h.count - min(h.count, 12) + i
                if GBEngineBridge.jump(toHistory: realIndex) {
                    self.render(GBEngineBridge.currentStep())
                }
            })
        }
        a.addAction(UIAlertAction(title: "取消", style: .cancel))
        present(a, animated: true)
    }

    @objc private func onAuto() {
        autoOn.toggle()
        autoTimer?.invalidate()
        if autoOn {
            toast("AUTO 已开启")
            autoTimer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in
                guard let self = self else { return }
                if self.typeTimer != nil { self.finishTyping(); return }
                let t = self.step["type"] as? String ?? ""
                if t == "Choice" || t == "End" { self.onAuto(); return }
                self.advanceOne()
            }
        } else { toast("AUTO 已关闭") }
    }

    @objc private func onSkip() {
        skipOn.toggle()
        skipTimer?.invalidate()
        if skipOn {
            toast("SKIP：跳过已读文本")
            skipTimer = Timer.scheduledTimer(withTimeInterval: 0.09, repeats: true) { [weak self] _ in
                guard let self = self else { return }
                let visited = self.step["visited"] as? Bool ?? false
                let t = self.step["type"] as? String ?? ""
                if !visited || t == "Choice" || t == "End" {
                    self.skipOn = false
                    self.skipTimer?.invalidate()
                    self.toast("遇到未读文本，已停止跳过")
                    return
                }
                self.finishTyping()
                self.advanceOne()
            }
        } else { toast("SKIP 已关闭") }
    }

    private func saveSlot(_ i: Int) {
        do {
            let dir = try saveDir()
            let url = dir.appendingPathComponent("slot\(i).txt")
            try GBEngineBridge.saveState().write(to: url, atomically: true, encoding: .utf8)
            toast("已存到槽位 \(i + 1)")
        } catch { toast("存档失败：\(error.localizedDescription)") }
    }

    private func loadSlot(_ i: Int) {
        do {
            let url = try saveDir().appendingPathComponent("slot\(i).txt")
            let s = try String(contentsOf: url, encoding: .utf8)
            if GBEngineBridge.restoreState(s) {
                render(GBEngineBridge.currentStep())
                toast("已读取槽位 \(i + 1)")
            } else { toast("读档失败：\(GBEngineBridge.lastError())") }
        } catch { toast("这个槽位还没有存档") }
    }

    private func saveDir() throws -> URL {
        let dir = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("saves", isDirectory: true)
        if !FileManager.default.fileExists(atPath: dir.path) {
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        }
        return dir
    }

    private func stopAllTimers() {
        typeTimer?.invalidate(); typeTimer = nil
        autoTimer?.invalidate(); autoTimer = nil
        skipTimer?.invalidate(); skipTimer = nil
        autoOn = false
        skipOn = false
    }

    private func toast(_ msg: String) {
        toastLabel.text = "  " + msg + "  "
        toastLabel.alpha = 1
        UIView.animate(withDuration: 0.35, delay: 1.4, options: .curveEaseOut) {
            self.toastLabel.alpha = 0
        }
    }

    private func showError(_ msg: String) {
        nameLabel.text = "引擎错误"
        finishTyping()
        textLabel.text = msg
        toast(msg)
    }
}
