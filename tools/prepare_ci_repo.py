#!/usr/bin/env python3
"""Build a clean, already-committed git repo that can be pushed straight to GitHub.

Why this exists: compiling an iOS arm64 app requires macOS/Xcode. On Windows the
only free route is a GitHub Actions macOS runner. This script produces the exact
file set that runner needs -- with no Web prototype, no WKWebView shell, no
legacy workflow -- and commits it so the user only has to add a remote and push.
"""
import os
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "dist", "GalBoxNative-CI-repo")
OUT_ZIP = os.path.join(ROOT, "dist", "GalBoxNative-CI-repo.zip")
NAME = "GalBoxNative-CI-repo"

CORE = [
    ".github/workflows/build-native-ipa.yml",
    "native/Engine/ByteUtils.hpp",
    "native/Engine/XP3Archive.hpp",
    "native/Engine/XP3Archive.cpp",
    "native/Engine/KagEngine.hpp",
    "native/Engine/KagEngine.cpp",
    "native/Tests/engine_cli.cpp",
    "native/Tests/win32_zlib_stub.cpp",
    "native/Tests/include/zlib.h",
    "tools/xp3_fixture.py",
    "tools/verify_native.py",
    "tools/verify_ipa.py",
    "tools/build_native.sh",
    "tools/prepare_native.py",
    "tools/prepare_ci_repo.py",
    "tests/native.test.py",
    "tests/ci.test.py",
    "ci.html",
]

GITIGNORE = """# 本地构建产物不入库（CI 会重新生成）
build-native/
__pycache__/
*.pyc
*.ipa
.DS_Store

# 注意：dist/fixtures/ 故意入库，它是 C++ 引擎自检的独立参考夹具。
"""

BUILD_TXT = """GalBox 原生引擎 · 没有 Mac 怎么出 .ipa
========================================

这是一份已经 git init + commit 好的干净仓库，只包含原生 App 工程和 CI 出包脚本，
没有网页原型、没有 WKWebView 壳。编译 iOS arm64 程序必须用 macOS + Xcode，
本机 Windows 做不到；GitHub Actions 的 macOS runner 可以，公开仓库免费。

【第一步】在 github.com 新建一个空仓库（Public，不要勾选 Add a README）
         记下仓库地址，例如 https://github.com/你的用户名/galbox-native.git

【第二步】在本目录打开终端，执行两条命令：
         git remote add origin https://github.com/你的用户名/galbox-native.git
         git push -u origin main
         推送需要 GitHub 登录：推荐装 GitHub Desktop 图形化登录，
         或在 GitHub 设置里生成 Personal Access Token 代替密码。

【第三步】打开仓库的 Actions 页，会出现
         「Build GalBox Native iOS IPA」
         首次 push 会自动开始，也可以点 Run workflow 手动重跑，约 5-15 分钟。

【第四步】构建成功后，在页面下方 Artifacts 下载
         GalBoxNative-ios27-sideload
         里面 GalBoxNative-adhoc.ipa 就是要用的包（另一份 unsigned 是裸包备份）。
         Artifact 保留 30 天，请及时下载。

【第五步】装到 iOS 27：这一步不需要 Mac。
         在 Windows 上装 Sideloadly，连上 iPhone，选 GalBoxNative-adhoc.ipa，
         填免费 Apple ID 重签安装；AltStore / SideStore 同理。
         免费签名 7 天有效，到期用同一工具重签一次即可。
         iOS 27 上 TrollStore / .tipa 不可用（CoreTrust 漏洞已在 17.0.1 修复）。

如果构建失败
  - 先看 Artifact「GalBoxNative-build-log」里的 xcodebuild.log。
  - 若报 SDK/Xcode 相关：Actions 页面点 Run workflow，在 xcode 输入框里
    填日志开头「可用 Xcode」列出的某个目录名（如 Xcode_26.1.app）后重跑。
  - 部署目标是 iOS 15.0，用任意较新 SDK 编出的 arm64 包都能在 iOS 27 上跑，
    不必强求 SDK 正好是 27。

本机自检（不需要 Mac，随时可跑）
  python tools/xp3_fixture.py
  python tools/verify_native.py      # 真编译并运行 C++ 引擎
  python tests/native.test.py        # 原生工程静态校验
  python tests/ci.test.py            # CI 出包链路 + 假包闭环校验

能力边界：支持真实 XP3 归档与 KAG 剧本子集；不含 TJS2 虚拟机、完整 KAG
指令集、音视频解码、商用加密包滤镜。要 100% 兼容商业 galgame，
正确做法是编译 zeas2/Kirikiroid2 源码。

Bundle ID: com.galbox.native   最低系统: iOS 15.0   架构: arm64
"""


def run(args, cwd=None):
    r = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(" ".join(args) + "\n" + r.stdout[-1500:] + r.stderr[-1500:])
    return r.stdout.strip()


def main():
    # Never delete anything outside our own dist output directory.
    if os.path.isdir(OUT_DIR):
        assert os.path.commonpath([OUT_DIR, os.path.join(ROOT, "dist")]) == \
            os.path.join(ROOT, "dist"), "拒绝删除 dist 之外的目录"
        shutil.rmtree(OUT_DIR)
    os.makedirs(OUT_DIR)

    files = list(CORE)
    for base, _dirs, names in os.walk(os.path.join(ROOT, "ios-native")):
        for n in names:
            files.append(os.path.relpath(os.path.join(base, n), ROOT).replace("\\", "/"))
    fixdir = os.path.join(ROOT, "dist", "fixtures")
    for n in sorted(os.listdir(fixdir)):
        if os.path.isfile(os.path.join(fixdir, n)):
            files.append("dist/fixtures/" + n)

    missing = [f for f in files if not os.path.isfile(os.path.join(ROOT, f))]
    if missing:
        print("ERROR 缺文件: " + ", ".join(missing))
        return 1

    for rel in sorted(set(files)):
        dst = os.path.join(OUT_DIR, *rel.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(os.path.join(ROOT, rel), dst)
    with open(os.path.join(OUT_DIR, ".gitignore"), "w", encoding="utf-8") as f:
        f.write(GITIGNORE)
    with open(os.path.join(OUT_DIR, "BUILD.txt"), "w", encoding="utf-8") as f:
        f.write(BUILD_TXT)

    # A fresh repo, so no hooks / identity config exists yet.
    try:
        run(["git", "init", "-b", "main"], cwd=OUT_DIR)
    except RuntimeError:
        run(["git", "init"], cwd=OUT_DIR)
        run(["git", "symbolic-ref", "HEAD", "refs/heads/main"], cwd=OUT_DIR)
    run(["git", "add", "-A"], cwd=OUT_DIR)
    run(["git", "-c", "user.name=GalBox CI", "-c", "user.email=ci@galbox.local",
         "commit", "-m", "GalBox native iOS app + no-Mac GitHub Actions build"],
        cwd=OUT_DIR)
    tracked = run(["git", "ls-files"], cwd=OUT_DIR).splitlines()
    tracked = [t.replace("\\", "/") for t in tracked]

    required = [
        ".github/workflows/build-native-ipa.yml",
        "ios-native/GalBoxNative.xcodeproj/project.pbxproj",
        "ios-native/GalBoxNative/GBEngineBridge.mm",
        "native/Engine/XP3Archive.cpp",
        "tools/build_native.sh",
        "tools/verify_ipa.py",
        "tests/ci.test.py",
        "dist/fixtures/manifest.json",
        "ci.html",
        "BUILD.txt",
    ]
    for r in required:
        assert r in tracked, "仓库缺 " + r
    banned = ("index.html", "app.js", "engine.js", "data.js", "install.html",
              "build-ipa.yml", "GalBox-shell.ipa")
    for t in tracked:
        assert not any(b in t for b in banned), "仓库混入无关文件 " + t
    branch = run(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=OUT_DIR)
    assert branch == "main", "默认分支应为 main，实际 " + branch

    # Bundle the .git directory too: unzip -> add remote -> push, no git init needed.
    with zipfile.ZipFile(OUT_ZIP, "w", zipfile.ZIP_DEFLATED) as z:
        for base, _dirs, names in os.walk(OUT_DIR):
            for n in names:
                full = os.path.join(base, n)
                rel = os.path.relpath(full, OUT_DIR).replace("\\", "/")
                z.write(full, NAME + "/" + rel)

    print("OK  仓库目录: %s" % OUT_DIR)
    print("OK  仓库 zip : %s (%d bytes)" % (OUT_ZIP, os.path.getsize(OUT_ZIP)))
    print("OK  已提交 %d 个文件，分支 main，HEAD: %s" %
          (len(tracked), run(["git", "rev-parse", "--short", "HEAD"], cwd=OUT_DIR)))
    print("    下一步: git remote add origin <你的仓库地址> && git push -u origin main")
    return 0


if __name__ == "__main__":
    sys.exit(main())
