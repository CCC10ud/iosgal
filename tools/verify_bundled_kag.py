#!/usr/bin/env python3
"""核验 app bundle 里内置的 demo-game.xp3 是不是**真能跑**的 KAG 包。

为什么需要这一步：出包链路里的 strings 标记只能证明 C++ 代码链接进包，
证明不了「包里有脚本可执行」。M2e-2~M2e-12 做的 Layer/Window/Menu/Timer/
Font/KAGParser/AsyncTrigger 骨架，一度只存在于 Linux 自测夹具，内置包里的
startup.tjs 只有一行注释 —— 自测全绿而设备上根本没跑过官方启动链。

判据（任一不满足即失败）：
  1. startup.tjs 存在，且正文里真的 `execStorage("system/Initialize.tjs")`；
  2. system/Initialize.tjs 是官方原文（体积下限，探针版只有几百字节）；
  3. 九个 GalBox 骨架文件全在包内（LoadEngineShim 靠 IsExistentStorage 找它们，
     少一个就在设备上悄悄退回缺类状态）；
  4. 官方 Config.tjs 在包内（KAGWindow_config 定义在它里面）；
  5. 条目数与总体积达到下限 —— 防止「文件都在但内容被截断」；
  6. KAGParser 骨架里带 `var debugLevel`（M2e-13）—— 官方 AnimationLayer.tjs:266
     给该成员赋值，缺了设备端就停在动画层构造；
  7. Layer 骨架里带 `var hitType` / `var hitThreshold`（M2e-14）——
     官方 GraphicLayer.tjs:337-338 给它们赋值。
  8. Layer 骨架里带 `function setImageSize` / `function setSizeToImageSize`
     （M2e-15）—— 官方 MainWindow.tjs:348-353 建背景层时连调这两个方法。
  9. Layer 骨架里带绘制面 `var face` / `function fillRect`（M2e-16）——
     官方 GraphicLayer.tjs:422-423 的 clearImage() 先设绘制面再填色。
 10. Layer 骨架里带 `var cursor = crDefault`（M2e-17）—— 官方
     GraphicLayer.tjs:58 的 setDefaultCursor() 给该成员赋值。
 11. Layer 骨架里带 `var name = ""`（M2e-18）—— 官方 HistoryLayer.tjs:110
     给图层名赋值，MainWindow.tjs:3219 拿它做字符串拼接与 conductor 键。
 12. Layer 骨架里带 `var focusable = false`（M2e-19）—— 官方
     HistoryLayer.tjs:130 构造时写 true，MessageLayer.tjs:2316 拿它做取反判断。
 13. Layer 骨架里带 `function setSize`（M2e-20）—— 官方
     AnimationLayer.tjs:367 的 clearImage() 首句 `setSize(32, 32)`。
 14. Layer 骨架里带 `var imageModified = true`（M2e-21）—— 官方
     MessageLayer.tjs:493 构造时置 true，562 `if(imageModified)` 真读。
 15. Layer 骨架里带 `function colorRect`（M2e-22）—— 官方
     MessageLayer.tjs:587 的 clear() 首句 `colorRect(0,0,imageWidth,imageHeight,0)`。
 16. Layer 骨架里带 `var showParentHint = true`（M2e-23）—— 官方
     MessageLayer.tjs:1987 link 隐藏路径写 false（全库零读点，取原生默认 true）。
 17. Layer 骨架里带 `var cursorX = 0; var cursorY = 0`（M2e-24）—— 官方
     MessageLayer.tjs:607 clear() 记录鼠标位置，MainWindow.tjs:4006 跨对象读。
 18. 内置包带 `system/GalBoxSound.tjs` 含 `class MIDISoundBuffer`（M2e-25）
     —— BGM.tjs:114 `class KAGMIDISoundBuffer extends MIDISoundBuffer, ...`。
 19. 同一文件含 `class WaveSoundBuffer`（M2e-26）—— SE.tjs:4
     `class SESoundBuffer extends WaveSoundBuffer` / SE.tjs:19 `super.WaveSoundBuffer(owner)`。
 20. 内置包带 `system/GalBoxMovie.tjs` 含 `class VideoOverlay`（M2e-27）
     —— Movie.tjs:4 `class Movie extends VideoOverlay`。
 21. Window 骨架里带 `var left = 0; var top = 0`（M2e-28）—— 官方
     MainWindow.tjs:438 `this.width + this.left`（窗口居中修正）。
 22. Sound 骨架（MIDI + Wave 两个类）各带 `var paused = false`（M2e-29）——
     BGM.tjs:585 `dic.paused = buf1.paused;` / 309 `buf.paused = paused;`。
 23. WaveSoundBuffer 带 `var pan = 0`（M2e-30）—— SE.tjs:213-223 的
     `property pan` 读写 `super.pan`（单位 1/1000）。
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xp3_fixture as X  # noqa: E402

SKELETONS = ["Consts", "Font", "Layer", "Window", "Pad", "Menu", "Timer",
             "KAGParser", "AsyncTrigger", "Sound", "Movie"]
REQUIRED = ["startup.tjs", "system/Initialize.tjs", "system/Config.tjs"] + \
           ["system/GalBox%s.tjs" % s for s in SKELETONS]

MIN_ENTRIES = 30
MIN_INIT_BYTES = 1000     # 官方 Initialize.tjs 约 7 KB；探针版远小于此
MIN_TOTAL_BYTES = 100000  # 20 个官方脚本 + Config 的压缩体量下限


def fail(msg):
    print("::error::" + msg)
    return 1


def main(argv):
    if len(argv) != 2:
        raise SystemExit("用法: verify_bundled_kag.py <app 内的 demo-game.xp3>")
    path = argv[1]
    if not os.path.isfile(path):
        return fail("找不到内置包: %s" % path)

    raw = open(path, "rb").read()
    try:
        parsed = X.parse_xp3(raw, collect_data=True)
    except Exception as ex:
        return fail("内置包解析失败（不是合法 XP3）: %s" % ex)

    names = set(e["name"] for e in parsed["entries"])
    missing = [n for n in REQUIRED if n not in names]
    if missing:
        return fail("内置包缺文件: %s" % missing)

    if parsed["count"] < MIN_ENTRIES:
        return fail("内置包只有 %d 个文件（下限 %d）：官方系统脚本没打进去？"
                    % (parsed["count"], MIN_ENTRIES))

    by_name = {e["name"]: e for e in parsed["entries"]}
    init_bytes = by_name["system/Initialize.tjs"]["orgSize"]
    if init_bytes < MIN_INIT_BYTES:
        return fail("system/Initialize.tjs 只有 %d B（下限 %d）：像是探针版而非官方原文"
                    % (init_bytes, MIN_INIT_BYTES))

    total = sum(e["orgSize"] for e in parsed["entries"])
    if total < MIN_TOTAL_BYTES:
        return fail("内置包解压后共 %d B（下限 %d）：内容被截断？"
                    % (total, MIN_TOTAL_BYTES))

    # startup.tjs 必须是官方那一句：只看「文件在」不够 —— 它曾经就在，
    # 只是内容是 `// demo game package` 一行注释。正文必须解压出来比，
    # 不能在压缩字节里瞎搜（压缩段搜不到任何明文）。
    boot_text = by_name["startup.tjs"]["data"]
    if b"system/Initialize.tjs" not in boot_text:
        return fail("startup.tjs 没有 execStorage(\"system/Initialize.tjs\")，"
                    "仍是注释版：设备上不会跑官方启动链")
    if b"execStorage" not in boot_text:
        return fail("startup.tjs 里没有 execStorage 调用")

    # Config.tjs 必须带 KAGWindow_config（M2e-5 的核心缺口就来自它）。
    # CP932 里 ASCII 恒等，所以直接在解出的字节里搜函数名是可靠的。
    cfg = by_name["system/Config.tjs"]["data"]
    if b"KAGWindow_config" not in cfg:
        return fail("system/Config.tjs 里没有 KAGWindow_config：不是官方配置，"
                    "会在 MainWindow.tjs 撞缺类")

    # M2e-13：KAGParser 骨架必须带 `debugLevel` 这个**只写不读**的槽位。
    # 官方 AnimationLayer.tjs:266 / Conductor.tjs:350 在构造里给它赋值，缺了
    # 就抛 `Member "debugLevel" does not exist`，动画层构造整个失败。
    # 骨架是 UTF-8（ASCII 兼容），直接在解出的字节里搜子串即可。
    kp = by_name["system/GalBoxKAGParser.tjs"]["data"]
    if b"var debugLevel" not in kp:
        return fail("system/GalBoxKAGParser.tjs 里没有 var debugLevel："
                    "设备端会在 AnimationLayer.tjs:266 断链")

    # M2e-14：Layer 骨架必须带命中测试面（hitType / hitThreshold）。
    # 官方 GraphicLayer.tjs:337-338 在 BaseLayer 构造里连写这两个成员。
    layer = by_name["system/GalBoxLayer.tjs"]["data"]
    for token in (b"var hitType", b"var hitThreshold"):
        if token not in layer:
            return fail("system/GalBoxLayer.tjs 里没有 %s："
                        "设备端会在 GraphicLayer.tjs:337 断链" % token.decode())

    # M2e-15：Layer 骨架必须带图像面（imageWidth/imageHeight + 两个方法）。
    # 官方 MainWindow.tjs:348-353 建表/裏两张背景层：先 setImageSize() 写图像
    # 尺寸，再 setSizeToImageSize() 把图层尺寸对齐过去。两者缺一，MainWindow
    # 构造就断在 348/349。imageWidth/imageHeight 也必须真存在 —— KAGLayer 的
    # store() 会原样拷走它们（void 会一路污染 restore()）。
    for token in (b"var imageWidth", b"var imageHeight",
                  b"function setImageSize", b"function setSizeToImageSize"):
        if token not in layer:
            return fail("system/GalBoxLayer.tjs 里没有 %s："
                        "设备端会在 MainWindow.tjs:348 断链" % token.decode())

    # M2e-16：Layer 骨架必须带绘制面（face + fillRect）。官方
    # GraphicLayer.tjs:422-423 的 clearImage() 是 `face = dfAlpha;` 后紧接
    # `fillRect(0, 0, imageWidth, imageHeight, 0xff000000)`，由
    # MainWindow.tjs:356 的 freeImage() 触发。face 的初值必须是 dfAuto ——
    # 官方 AnimationLayer.tjs:369-372 画完就 `face = dfAuto`，说明它才是常态。
    for token in (b"var face = dfAuto", b"function fillRect"):
        if token not in layer:
            return fail("system/GalBoxLayer.tjs 里没有 %s："
                        "设备端会在 GraphicLayer.tjs:422 断链" % token.decode())

    # 第 10 条（M2e-17）：光标面 cursor。GraphicLayer.tjs:58 的 setDefaultCursor()
    # 是 `cursor = defaultCursor = c;`，由 MainWindow.tjs:359-360 给表/裏两张
    # 背景层各调一次。cursor 是原生 Layer 成员，全脚本没有 var 声明
    # （GraphicLayer.tjs:13 那个属于 ProvinceContext）；初值必须是 crDefault ——
    # 官方 GraphicLayer.tjs:31 自己就是 `var defaultCursor = crDefault;`。
    if b"var cursor = crDefault" not in layer:
        return fail("system/GalBoxLayer.tjs 里没有 var cursor = crDefault："
                    "设备端会在 GraphicLayer.tjs:58 断链")

    # 第 11 条（M2e-18）：图层名 name。HistoryLayer.tjs:110 构造时赋值；
    # MainWindow.tjs:3219 `'anim:' + layer.name + ':' + seg` 拿它做字符串拼接
    # 与 conductor 键 ⇒ 必须是字符串真值，留 void 会拼出 (void) 或污染键名。
    # 官方两处 comp 互换（GraphicLayer.tjs:134 / MessageLayer.tjs:2913）
    # 也要求它是实例成员，不能做成全局。
    if b'var name = ""' not in layer:
        return fail("system/GalBoxLayer.tjs 里没有 var name = \"\"："
                    "设备端会在 HistoryLayer.tjs:110 断链")

    # 第 12 条（M2e-19）：焦点接收面 focusable。HistoryLayer.tjs:130 构造时
    # 写 `focusable = true;`；MessageLayer.tjs:2316 `if(!focusable || ...)` 真读值
    # ⇒ 必须是布尔真值，留 void 会 !void 为真、把所有可聚焦层判成不可聚焦。
    # MessageLayer.tjs:2605 `focusable = src.focusable;` 还要求它是实例成员。
    # 初值 false 取自官方 HistoryLayer.tjs:10 / MessageLayer.tjs:595。
    if b"var focusable = false;" not in layer:
        return fail("system/GalBoxLayer.tjs 里没有 var focusable = false："
                    "设备端会在 HistoryLayer.tjs:130 断链")

    # 第 13 条（M2e-20）：显示尺寸方法 setSize。AnimationLayer.tjs:367 的
    # clearImage() 首句 `setSize(32, 32);`；官方 ButtonLayer/EditLayer 覆写时
    # 都调 `super.setSize(w, h)` ⇒ 基类必须真写 width/height，不能是空壳。
    # 必须带函数体写 width/height，只搜函数名不足以排除空实现。
    if b"function setSize(w, h)" not in layer or b"width = w;" not in layer:
        return fail("system/GalBoxLayer.tjs 里没有 setSize(w,h) 写 width/height："
                    "设备端会在 AnimationLayer.tjs:367 断链")

    # 第 14 条（M2e-21）：图像脏标记 imageModified。MessageLayer.tjs:493 构造时
    # 置 true，562 `if(imageModified)` 真读值 ⇒ 必须是布尔真值，留 void 会
    # if 恒假、跳过图像重绘且不报错。初值 true = "新层图像待重绘"。
    if b"var imageModified = true;" not in layer:
        return fail("system/GalBoxLayer.tjs 里没有 var imageModified = true："
                    "设备端会在 MessageLayer.tjs:493 断链")

    # 第 15 条（M2e-22）：颜色矩形填充 colorRect。MessageLayer.tjs:587 的
    # clear() 里 `colorRect(0, 0, imageWidth, imageHeight, 0);`（mode 省略）。
    # 必须带完整参数列表匹配，防与 fillRect(M2e-16) 混淆——两者只差一个字母。
    if b"function colorRect(l, t, w, h, color, mode)" not in layer:
        return fail("system/GalBoxLayer.tjs 里没有 colorRect(l,t,w,h,color,mode)："
                    "设备端会在 MessageLayer.tjs:587 断链")

    # 第 16 条（M2e-23）：父层提示开关 showParentHint。MessageLayer.tjs:1987
    # 的 link 隐藏路径 `showParentHint = false;`。全库零读点，但必须可写，
    # 留着“成员不存在”会在 1987 断链。
    if b"var showParentHint = true;" not in layer:
        return fail("system/GalBoxLayer.tjs 里没有 var showParentHint = true："
                    "设备端会在 MessageLayer.tjs:1987 断链")

    # 第 17 条（M2e-24）：层内鼠标坐标 cursorX/cursorY（成对成员）。
    # MessageLayer.tjs:607 读 + MainWindow.tjs:4006 跨对象读 ⇒ 必须是
    # 实例数值成员。核验器只查 cursorX（cursorY 与它同段声明，删一个必删另一个）。
    if b"var cursorX = 0;" not in layer or b"var cursorY = 0;" not in layer:
        return fail("system/GalBoxLayer.tjs 里没有 var cursorX/cursorY = 0："
                    "设备端会在 MessageLayer.tjs:607 断链")

    # 第 18 条（M2e-25）：音频类骨架 MIDISoundBuffer。BGM.tjs:114 的
    # `class KAGMIDISoundBuffer extends MIDISoundBuffer, KAGSoundBuffer`
    # 需要全局类可解析。GalBoxSound.tjs 必须在包里且含类定义。
    snd_name = "system/GalBoxSound.tjs"
    if snd_name not in by_name:
        return fail("内置包里没有 %s：设备端会在 BGM.tjs:114 断链" % snd_name)
    snd = by_name[snd_name]["data"]
    if b"class MIDISoundBuffer" not in snd:
        return fail("%s 里没有 class MIDISoundBuffer："
                    "设备端会在 BGM.tjs:114 断链" % snd_name)

    # 第 19 条（M2e-26）：同族的 WaveSoundBuffer（WAV/OGG 播放基类）。
    # SE.tjs:4 `class SESoundBuffer extends WaveSoundBuffer`；
    # SE.tjs:19 `super.WaveSoundBuffer(owner);`（super 链要能调到基类构造）。
    if b"class WaveSoundBuffer" not in snd:
        return fail("%s 里没有 class WaveSoundBuffer："
                    "设备端会在 SE.tjs:4 断链" % snd_name)

    # 第 20 条（M2e-27）：影片类骨架 VideoOverlay。Movie.tjs:4
    # `class Movie extends VideoOverlay`，16 行 `super.VideoOverlay(...)`。
    mov_name = "system/GalBoxMovie.tjs"
    if mov_name not in by_name:
        return fail("内置包里没有 %s：设备端会在 Movie.tjs:4 断链" % mov_name)
    mov = by_name[mov_name]["data"]
    if b"class VideoOverlay" not in mov:
        return fail("%s 里没有 class VideoOverlay："
                    "设备端会在 Movie.tjs:4 断链" % mov_name)
    # 第 24 条（M2e-31）：播放模式 mode（Movie.tjs:30 `dic.mode = mode;`）。
    if b"var mode = vomLayer;" not in mov:
        return fail("%s 里没有 var mode = vomLayer："
                    "设备端会在 Movie.tjs:30 断链" % mov_name)
    # 第 25 条（M2e-33）：影片可见性 visible（Movie.tjs:33 `dic.visible = visible;`，
    # 253 行 restore() 回写）。裸标识符读写 ⇒ 普通实例成员，var 即可。
    if b"var visible = true;" not in mov:
        return fail("%s 里没有 var visible = true："
                    "设备端会在 Movie.tjs:33 断链" % mov_name)
    # 第 26 条（M2e-34）：循环播放标志 loop（Movie.tjs:34 `dic.loop = loop;`，
    # 65 行 restore() 回写）。裸标识符读写 ⇒ 普通实例成员，var 即可。
    if b"var loop = false;" not in mov:
        return fail("%s 里没有 var loop = false："
                    "设备端会在 Movie.tjs:34 断链" % mov_name)
    # 第 27 条（M2e-35）：当前播放帧号 frame（Movie.tjs:35 `dic.frame = frame;`，
    # 89 行 restore() 回写）。裸标识符读写 ⇒ 普通实例成员，var 即可。
    if b"var frame = 0;" not in mov:
        return fail("%s 里没有 var frame = 0："
                    "设备端会在 Movie.tjs:35 断链" % mov_name)

    # 第 21 条（M2e-28）：窗口左上角坐标。MainWindow.tjs:438 的
    # `this.width + this.left`（居中修正）要读 Window.left；439 行还会回写。
    # 注意别与 Layer.left（M2e-2 已有）混淆：这里查的是 GalBoxWindow.tjs。
    win = by_name["system/GalBoxWindow.tjs"]["data"]
    if b"var left = 0;" not in win or b"var top = 0;" not in win:
        return fail("system/GalBoxWindow.tjs 里没有 var left/top = 0："
                    "设备端会在 MainWindow.tjs:438 断链")

    # 第 22 条（M2e-29）：音频缓冲区暂停位 paused。MIDI 与 Wave 两个类
    # 各要一份（BGM 的 buf1 可能是任一类型）；差一份就是静默丢状态。
    if snd.count(b"var paused = false;") < 2:
        return fail("%s 里 var paused = false 少于 2 处："
                    "MIDI/Wave 两个缓冲区类各要一份，否则 BGM.tjs:585 断链" % snd_name)

    # 第 23 条（M2e-30）：声像 pan。SE.tjs:217 `super.pan = x * 1000;`、
    # 221 `return super.pan \ 1000;` ⇒ 基类必须有整数 pan（单位 1/1000）。
    # 必须是 property（类对象上的访问器），光有 var 字段时 super.pan 解析不到
    # （run 44319521519 实测：A 仍停在 SE.tjs:221）。
    if b"property pan" not in snd or b"var panValue" not in snd:
        return fail("%s 里没有 property pan + var panValue："
                    "设备端会在 SE.tjs:221 断链" % snd_name)

    print("[m2e12b] 内置包核验通过: %d 个文件 / 解压后 %d B / "
          "Initialize.tjs %d B / startup.tjs 真调 Initialize / %d 个骨架全在 / "
          "KAGParser 带 debugLevel(M2e-13) / Layer 带 hitType+hitThreshold(M2e-14) / "
          "Layer 带 setImageSize+setSizeToImageSize(M2e-15) / "
          "Layer 带 face+fillRect(M2e-16) / Layer 带 cursor(M2e-17) / "
          "Layer 带 name(M2e-18) / Layer 带 focusable(M2e-19) / "
          "Layer 带 setSize(M2e-20) / "
          "Layer 带 imageModified(M2e-21) / Layer 带 colorRect(M2e-22) / "
          "Layer 带 showParentHint(M2e-23) / Layer 带 cursorX+cursorY(M2e-24) / "
          "Sound 骨架 MIDISoundBuffer(M2e-25) / WaveSoundBuffer(M2e-26) / "
          "Movie 骨架 VideoOverlay+mode+visible+loop+frame(M2e-27/31/33/34/35) / Window 带 left+top(M2e-28) / "
          "Sound 带 paused×2(M2e-29) / Wave 带 pan property(M2e-30) / "
          "Scripts.evalStorage 原生方法(M2e-32，二进制内)"
          % (parsed["count"], total, init_bytes, len(SKELETONS)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
