#!/usr/bin/env python3
"""Compile and actually run the native C++ engine core.

This is the real closed-loop check: generate legal XP3 archives with
xp3_fixture.py, compile XP3Archive.cpp / KagEngine.cpp / engine_cli.cpp,
then compare the CLI's JSON against the independent Python reference reader.

Works on macOS/Linux with clang++ and -lz, and on Windows with a bundled
toolchain (ziglang) plus a shim that forwards to the system zlib1.dll.
If no compiler exists, it prints SKIP and exits 3 -- it never claims success.
"""
import glob
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = os.path.join(ROOT, "dist", "fixtures")
WIN = platform.system() == "Windows"


def log(msg):
    print(msg, flush=True)


def find_msvc():
    """Return (cl.exe, msvc_root) from a Visual Studio BuildTools install."""
    pats = [
        "C:/Program Files (x86)/Microsoft Visual Studio/*/BuildTools/VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe",
        "C:/Program Files/Microsoft Visual Studio/*/*/VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe",
        "C:/Program Files (x86)/Microsoft Visual Studio/*/*/VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe",
    ]
    hits = []
    for pat in pats:
        hits += glob.glob(pat)
    if not hits:
        return None
    cl = sorted(hits)[-1]
    norm = cl.replace("\\", "/")
    root = norm.split("/bin/Hostx64/")[0]
    return cl, root


def msvc_env(cl, msvc_root, tmp):
    """Build INCLUDE/LIB explicitly from the installed MSVC + Windows SDK.

    vcvars64.bat calls reg.exe, which is blocked by the sandbox policy here,
    so the compiler environment is assembled from the actual install paths.
    """
    kits = sorted(glob.glob("C:/Program Files (x86)/Windows Kits/10/Lib/*/um/x64"))
    if not kits:
        return None
    kitver = os.path.basename(os.path.dirname(os.path.dirname(kits[-1])))
    kit = "C:/Program Files (x86)/Windows Kits/10"
    inc = [msvc_root + "/include",
           kit + "/Include/" + kitver + "/ucrt",
           kit + "/Include/" + kitver + "/um",
           kit + "/Include/" + kitver + "/shared"]
    lib = [msvc_root + "/lib/x64",
           kit + "/Lib/" + kitver + "/ucrt/x64",
           kit + "/Lib/" + kitver + "/um/x64"]
    env = dict(os.environ)
    env["INCLUDE"] = ";".join(os.path.normpath(p) for p in inc)
    env["LIB"] = ";".join(os.path.normpath(p) for p in lib)
    env["PATH"] = os.path.dirname(cl).replace("\\", "/") + ";" + env.get("PATH", "")
    env["TMP"] = tmp
    env["TEMP"] = tmp
    log("  INCLUDE[0]=" + inc[0])
    return env


def compiler():
    """Return (kind, command). kind is 'gnu' or 'msvc'."""
    for name in ("clang++", "g++", "c++"):
        p = shutil.which(name)
        if p:
            return "gnu", [p]
    m = find_msvc()
    if m:
        return "msvc", list(m)
    try:
        import ziglang  # noqa: F401
        return "gnu", [sys.executable, "-m", "ziglang", "c++"]
    except Exception:
        return None


def find_zlib_dll():
    pats = [
        "C:/Program Files/Git/mingw64/bin/zlib1.dll",
        "C:/Program Files (x86)/Git/mingw64/bin/zlib1.dll",
        "C:/msys64/mingw64/bin/zlib1.dll",
    ]
    pats += glob.glob("C:/Program Files/Git*/mingw64/bin/zlib1.dll")
    for p in pats:
        if os.path.isfile(p):
            return p
    cyg = shutil.which("cygpath")
    if cyg:
        try:
            out = subprocess.check_output([cyg, "-w", "/mingw64/bin/zlib1.dll"],
                                          text=True).strip()
            if os.path.isfile(out):
                return out
        except Exception:
            pass
    return None


def build(cc, tmp):
    kind, cc_cmd = cc
    exe = os.path.join(tmp, "engine_cli.exe" if WIN else "engine_cli")
    srcs = [
        os.path.join(ROOT, "native/Engine/XP3Archive.cpp"),
        os.path.join(ROOT, "native/Engine/KagEngine.cpp"),
        os.path.join(ROOT, "native/Tests/engine_cli.cpp"),
    ]
    env = None
    if kind == "msvc":
        cl, msvc_root = cc_cmd
        env = msvc_env(cl, msvc_root, tmp)
        if not env:
            log("编译失败: 找不到 Windows Kits SDK 库目录")
            return None
        cmd = [cl, "/nologo", "/EHsc", "/std:c++14", "/W1", "/utf-8",
               "/D_CRT_SECURE_NO_WARNINGS", "/D_WIN32",
               "/I", os.path.join(ROOT, "native/Tests/include"),
               "/I", os.path.join(ROOT, "native/Engine")]
        if WIN:
            srcs.append(os.path.join(ROOT, "native/Tests/win32_zlib_stub.cpp"))
        cmd += srcs + ["/Fe:" + exe, "/Fo:" + tmp + os.sep]
        log("编译: MSVC cl.exe (BuildTools) ...")
    else:
        cmd = list(cc_cmd) + ["-std=c++14", "-O0", "-g", "-Wall",
                              "-Wno-unused-parameter"]
        if WIN:
            cmd += ["-I", os.path.join(ROOT, "native/Tests/include")]
            srcs.append(os.path.join(ROOT, "native/Tests/win32_zlib_stub.cpp"))
        else:
            cmd += ["-I", os.path.join(ROOT, "native/Engine")]
        cmd += srcs + ["-o", exe]
        if not WIN:
            cmd.append("-lz")
        log("编译: " + " ".join(os.path.basename(x) for x in cmd[:4]) + " ...")
    # MSVC writes GBK diagnostics on Chinese Windows; never let a decode error
    # hide the real compiler result.
    r = subprocess.run(cmd, capture_output=True, env=env)
    out = (r.stdout or b"").decode("utf-8", errors="replace")
    err = (r.stderr or b"").decode("utf-8", errors="replace")
    if r.returncode != 0:
        log("编译失败:\n" + out[-2500:] + "\n" + err[-2500:])
        return None
    if WIN:
        dll = find_zlib_dll()
        if dll:
            shutil.copyfile(dll, os.path.join(tmp, "zlib1.dll"))
            log("已复制 zlib1.dll 到构建目录")
        else:
            log("警告: 未找到 zlib1.dll，压缩段测试可能失败")
    return exe


def run(exe, *args):
    r = subprocess.run([exe] + list(args), capture_output=True, text=True)
    return r.returncode, r.stdout.strip(), r.stderr.strip()


def main():
    if not os.path.isfile(os.path.join(FIX, "manifest.json")):
        subprocess.check_call([sys.executable,
                               os.path.join(ROOT, "tools/xp3_fixture.py")],
                              stdout=subprocess.DEVNULL)
    manifest = json.load(open(os.path.join(FIX, "manifest.json"), encoding="utf-8"))
    cc = compiler()
    if not cc:
        log("SKIP: 本机没有可用的 C++ 编译器（clang++/g++/MSVC/ziglang）。")
        log("      静态校验仍由 tests/native.test.py 覆盖；真编译请在 macOS 或")
        log("      安装 pip install ziglang 后重跑本脚本。")
        return 3

    passed = failed = 0

    def ok(cond, name):
        nonlocal passed, failed
        if cond:
            passed += 1
            log("  PASS " + name)
        else:
            failed += 1
            log("  FAIL " + name)

    with tempfile.TemporaryDirectory(prefix="galbox-native-") as tmp:
        exe = build(cc, tmp)
        if not exe:
            return 1

        log("\n[1] C++ 引擎自检（解析 / 变量 / 存档 / 回放）")
        rc, out, err = run(exe, "--selftest")
        for line in out.splitlines():
            if line.startswith("FAIL"):
                log("  " + line)
        results = [l for l in out.splitlines() if l.startswith("RESULT ")]
        ok(rc == 0 and not any(l.startswith("FAIL") for l in out.splitlines()),
           "selftest 无失败项")
        ok(bool(results) and results[0].split()[1].split("/")[0] ==
           results[0].split()[1].split("/")[1],
           "selftest 计数自洽 " + (results[0] if results else "(无 RESULT)"))

        log("\n[2] XP3 夹具与 C++ 解析器交叉比对")
        for fname, exp in manifest["fixtures"].items():
            rc, out, err = run(exe, "--xp3", os.path.join(FIX, fname))
            try:
                got = json.loads(out)
            except Exception as ex:
                ok(False, fname + " 输出不是合法 JSON: " + str(ex) + " " + err[-200:])
                continue
            ok(got.get("ok") is True, fname + " 打开并读完所有段")
            ok(got.get("count") == exp["count"],
               fname + " 文件数 " + str(got.get("count")) + " == " + str(exp["count"]))
            ge, ee = got.get("entries", []), exp["entries"]
            if len(ge) != len(ee):
                ok(False, fname + " 条目数不一致")
                continue
            for g, e in zip(ge, ee):
                same = (g["name"] == e["name"] and
                        g["orgSize"] == e["orgSize"] and
                        g["arcSize"] == e["arcSize"] and
                        g["hash"] == e["hash"] and
                        g["dataLen"] == e["dataLen"] and
                        g["adler"] == e["adler"] and
                        g["head"] == e["head"] and
                        [bool(x) for x in g["parts"]] == e["parts"] and
                        g["readOk"] is True)
                ok(same, fname + " / " + e["name"] + " 索引+解压字节一致")

        log("\n[3] 坏包必须被 C++ 解析器拒绝")
        for fname in manifest["broken"]:
            rc, out, err = run(exe, "--xp3", os.path.join(FIX, fname))
            try:
                got = json.loads(out)
            except Exception:
                got = {}
            ok(got.get("ok") is False,
               fname + " 被拒绝（" + str(got.get("error", ""))[:52] + "）")

        log("\n[4] 包内 .ks 剧本经 C++ 运行时走通")
        ks = os.path.join(FIX, manifest.get("kagScript", "first.ks"))
        rc, out, err = run(exe, "--kag", ks)
        try:
            got = json.loads(out)
        except Exception:
            got = {}
        steps = got.get("steps", [])
        types = [s.get("type") for s in steps]
        ok(got.get("ok") is True, "剧本载入并运行")
        ok("Choice" in types and any(s.get("options", 0) == 2 for s in steps),
           "走到双选项分支")
        ok("Say" in types and "End" in types, "有对话且能走到结局")
        ok("Error" not in types, "运行过程无引擎错误")

    log("\n========================================")
    log("真编译运行校验: %d PASS / %d FAIL" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
