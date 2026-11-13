// Command-line self-check for the GalBox native engine core.
// Build (macOS):  clang++ -std=c++14 -O0 -g native/Engine/*.cpp native/Tests/engine_cli.cpp -lz -o engine_cli
// Build (Win):    see tools/verify_native.sh (uses the bundled zlib shim)
//
//   engine_cli --xp3 <file>     dump XP3 index + decompressed data facts as JSON
//   engine_cli --kag <file>     parse a .ks and walk the runtime
//   engine_cli --selftest       exercise script/save/restore/replay invariants
#include "XP3Archive.hpp"
#include "KagEngine.hpp"
#include "ByteUtils.hpp"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace galbox;

static std::string jesc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back((char)c); }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o.push_back((char)c);
    }
    return o;
}

static std::string readFile(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return std::string();
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static int cmdXP3(const std::string& path) {
    XP3Archive arc;
    std::string err;
    if (!arc.open(path, &err)) {
        std::cout << "{\"ok\":false,\"error\":\"" << jesc(err) << "\"}\n";
        return 0;   // a rejected archive is a valid test result, not a crash
    }
    std::ostringstream o;
    o << "{\"ok\":true,\"count\":" << arc.count() << ",\"entries\":[";
    const std::vector<XP3Entry>& es = arc.entries();
    std::string readErrors;
    for (size_t i = 0; i < es.size(); ++i) {
        const XP3Entry& e = es[i];
        std::vector<uint8_t> bytes;
        std::string rerr;
        bool ok = arc.read(e.name, bytes, &rerr);
        if (!ok) readErrors += (readErrors.empty() ? "" : "; ") + e.name + ": " + rerr;
        std::string head;
        for (size_t k = 0; k < bytes.size() && k < 16; ++k) {
            static const char* d = "0123456789abcdef";
            head.push_back(d[bytes[k] >> 4]);
            head.push_back(d[bytes[k] & 15]);
        }
        if (i) o << ",";
        o << "{\"name\":\"" << jesc(e.name) << "\",\"orgSize\":" << e.orgSize
          << ",\"arcSize\":" << e.arcSize << ",\"hash\":" << e.hash
          << ",\"readOk\":" << (ok ? "true" : "false")
          << ",\"dataLen\":" << bytes.size()
          << ",\"adler\":" << adlerOf(bytes)
          << ",\"head\":\"" << head << "\",\"parts\":[";
        for (size_t k = 0; k < e.segments.size(); ++k) {
            if (k) o << ",";
            o << (e.segments[k].compressed ? 1 : 0);
        }
        o << "]}";
    }
    o << "]}";
    std::string json = o.str();
    if (!readErrors.empty()) {
        // A corrupt data segment is only discovered while reading, not while
        // parsing the index. Report it as a failed archive so tests can assert.
        const std::string key = "\"ok\":true";
        size_t p = json.find(key);
        if (p != std::string::npos) json.replace(p, key.size(), "\"ok\":false");
        json = json.substr(0, json.size() - 1) +
               ",\"error\":\"" + jesc(readErrors) + "\"}";
    }
    std::cout << json << "\n";
    return 0;
}

static const char* stepName(StepType t) {
    switch (t) {
        case StepType::Say: return "Say";
        case StepType::Narr: return "Narr";
        case StepType::Choice: return "Choice";
        case StepType::Bg: return "Bg";
        case StepType::Image: return "Image";
        case StepType::End: return "End";
        default: return "Error";
    }
}

static int cmdKag(const std::string& path) {
    std::string text = readFile(path);
    KagRuntime rt;
    std::string err;
    if (!rt.loadText(text, &err)) {
        std::cout << "{\"ok\":false,\"error\":\"" << jesc(err) << "\"}\n";
        return 1;
    }
    std::ostringstream o;
    o << "{\"ok\":true,\"nodes\":" << rt.script().nodes.size()
      << ",\"labels\":" << rt.script().labels.size() << ",\"steps\":[";
    int guard = 0, n = 0;
    while (++guard < 200) {
        Step s = rt.current();
        if (n++) o << ",";
        o << "{\"type\":\"" << stepName(s.type) << "\",\"who\":\"" << jesc(s.who)
          << "\",\"text\":\"" << jesc(s.text) << "\",\"bg\":\"" << jesc(s.storage)
          << "\",\"visited\":" << (s.visited ? "true" : "false")
          << ",\"options\":" << s.options.size() << "}";
        if (s.type == StepType::End || s.type == StepType::Error) break;
        Step nx = (s.type == StepType::Choice) ? rt.choose(0) : rt.advance();
        if (nx.type == StepType::Error) {
            o << ",{\"type\":\"Error\",\"text\":\"" << jesc(nx.message) << "\"}";
            break;
        }
    }
    o << "],\"vars\":{";
    bool firstVar = true;
    for (const auto& kv : rt.vars()) {
        if (!firstVar) o << ",";
        firstVar = false;
        o << "\"" << jesc(kv.first) << "\":" << kv.second;
    }
    o << "},\"nodesDump\":[";
    for (size_t i = 0; i < rt.script().nodes.size(); ++i) {
        const KagNode& nd = rt.script().nodes[i];
        if (i) o << ",";
        o << "{\"i\":" << i << ",\"t\":" << (int)nd.type
          << ",\"var\":\"" << jesc(nd.varName) << "\",\"expr\":\"" << jesc(nd.expr) << "\""
          << ",\"cmp\":" << nd.cmpOp << ",\"cv\":" << nd.cmpValue
          << ",\"text\":\"" << jesc(nd.text) << "\",\"target\":\"" << jesc(nd.target)
          << "\",\"else\":\"" << jesc(nd.elseTarget) << "\"}";
    }
    o << "],\"warnings\":[";
    for (size_t i = 0; i < rt.warnings().size(); ++i) {
        if (i) o << ",";
        o << "\"" << jesc(rt.warnings()[i]) << "\"";
    }
    o << "]}";
    std::cout << o.str() << "\n";
    return 0;
}

static int g_pass = 0, g_fail = 0;
static void check(bool cond, const std::string& name) {
    // Flush each line: if the runtime crashes, the last printed assertion
    // tells us exactly where, instead of losing everything in the buffer.
    if (cond) { ++g_pass; std::cout << "PASS " << name << std::endl; }
    else { ++g_fail; std::cout << "FAIL " << name << std::endl; }
}

static const char* kScript =
    "*start\n"
    "[bg storage=\"bg/room.png\"]\n"
    "【小樱】你来了。[l]\n"
    "[eval exp=\"f.clue+=1\"]\n"
    "[if exp=\"f.clue>=1\"][jump target=*good][endif]\n"
    "不该走到这一句。[l][s]\n"
    "*good\n"
    "[link target=*a]陪她走[endlink][r]\n"
    "[link target=*b]留下来[endlink][r]\n"
    "[s]\n"
    "*a\n【小樱】一起走。[l][s]\n"
    "*b\n【小樱】再坐会儿。[l][s]\n";

static int cmdSelfTest() {
    KagScript sc = parseKag(kScript);
    check(sc.nodes.size() > 8, "KAG 解析出节点");
    check(sc.labels.size() == 4, "识别 start/good/a/b 共 4 个标签");

    KagRuntime rt;
    std::string err;
    check(rt.load(sc, &err), "载入剧本" + (err.empty() ? "" : " (" + err + ")"));

    Step first = rt.current();
    check(first.type == StepType::Say, "首个停顿是对话");
    check(first.who == "\xe5\xb0\x8f\xe6\xa8\xb1", "【名字】被解析成说话人（UTF-8）");
    check(first.storage == "bg/room.png", "[bg] 作为效果随对话带出");
    check(first.visited == false, "首次阅读标记为未读");

    // Save/restore on the line that ACTUALLY has a pending background.
    // Checking an empty field against another empty field would prove nothing.
    std::string saveBg;
    rt.save(saveBg);
    KagRuntime rtBg;
    check(rtBg.load(sc, &err), "第二个实例载入剧本");
    check(rtBg.restore(saveBg, &err), "在有背景的句子上读档");
    check(rtBg.current().storage == "bg/room.png", "读档恢复背景（非空值真比对）");
    check(rtBg.history().size() == rt.history().size(), "读档没有重复写入历史");

    Step choice = rt.advance();
    check(choice.type == StepType::Choice, "[if] 为真后 [jump] 抵达选项标签");
    check(choice.options.size() == 2, "连续 [link] 合并成 2 个选项");
    check(choice.options[0].text == "\xe9\x99\xaa\xe5\xa5\xb9\xe8\xb5\xb0",
          "[link] 正文落在选项文字里（没有掉进旁白）");
    check(rt.vars().count("clue") == 1 && rt.vars().at("clue") == 1,
          "f.clue+=1 变量名正确且累加生效");

    std::string save;
    rt.save(save);
    size_t histBefore = rt.history().size();
    check(save.find("GBSAVE1") == 0, "存档头正确");

    KagRuntime rt2;
    check(rt2.load(sc, &err), "新实例载入同一剧本");
    check(rt2.restore(save, &err), "读档成功");
    check(rt2.history().size() == histBefore, "读档没有重复写入历史");
    check(rt2.current().pc == choice.pc, "读档回到同一句");
    check(rt2.current().type == StepType::Choice &&
          rt2.current().options.size() == 2, "读档后仍停在同一个双选项");
    check(rt2.vars().count("clue") == 1 && rt2.vars().at("clue") == 1,
          "读档恢复变量");

    Step picked = rt.choose(0);
    check(picked.type == StepType::Say && picked.text.find("\xe4\xb8\x80\xe8\xb5\xb7\xe8\xb5\xb0") != std::string::npos,
          "选第 1 项跳到对应结局分支");
    check(rt.history().size() == histBefore + 1, "选项分支写入一条历史");

    check(rt.jumpToHistory(1), "回放可跳回第 2 条历史");
    check(rt.history().size() == 2, "回放截断后续历史并重新记录当前句");

    rt.jumpToLabel("start");
    check(rt.current().visited == true, "回到已读句子时标记为已读");

    KagScript bad = parseKag("[quake time=200]\n测试[l][s]\n");
    check(!bad.warnings.empty(), "不支持的标签会给出 warning");

    // ---- 新增 KAG 特性（富文本 / select / macro / 表达式 / 跨文件）----
    // 内联富文本：解析为 runs 而非吞掉
    const char* kRich =
        "*start\n"
        "[color color=red]红字[color default]普通[r]\n"
        "[ruby text=\xe3\x81\xad\xe3\x81\x93]\xe7\x8c\xab[l][s]\n";
    {
        KagScript scR = parseKag(kRich);
        bool sawColor = false, sawRed = false, sawRuby = false, sawNeko = false;
        for (const auto& nd : scR.nodes) {
            if (nd.type != KagType::Say && nd.type != KagType::Narr) continue;
            for (const auto& r : nd.runs) {
                if (r.type == RunType::Color) { sawColor = true; if (r.attr == "red") sawRed = true; }
                if (r.type == RunType::Ruby) { sawRuby = true; if (r.text == "\xe3\x81\xad\xe3\x81\x93") sawNeko = true; }
            }
        }
        check(sawColor, "内联 [color] 解析为 Color 富文本 run");
        check(sawRed, "内联 [color color=red] 的属性值 red 被记录");
        check(sawRuby, "内联 [ruby] 解析为 Ruby 富文本 run");
        check(sawNeko, "内联 [ruby text=ねこ] 的注音被记录");
    }

    // [select] 选项块
    const char* kSelect =
        "*start\n"
        "选一个：[r]\n"
        "[select]\n"
        "[*\xe5\x8e\xbb\xe4\xb8\x9c\xe8\xbe\xb9|*east]\n"
        "[*\xe5\x8e\xbb\xe8\xa5\xbf\xe8\xbe\xb9|*west]\n"
        "[endselect]\n"
        "[s]\n"
        "*east\n\xe4\xb8\x9c\xe8\xbe\xb9。[l][s]\n"
        "*west\n\xe8\xa5\xbf\xe8\xbe\xb9。[l][s]\n";
    {
        KagScript scSel = parseKag(kSelect);
        int selIdx = -1;
        for (size_t i = 0; i < scSel.nodes.size(); ++i)
            if (scSel.nodes[i].type == KagType::Choice) { selIdx = (int)i; break; }
        check(selIdx >= 0, "[select] 生成 Choice 节点");
        if (selIdx >= 0) {
            const KagNode& cn = scSel.nodes[selIdx];
            check(cn.options.size() == 2, "[select] 收集到 2 个选项");
            if (cn.options.size() >= 2) {
                check(cn.options[0].text == "\xe5\x8e\xbb\xe4\xb8\x9c\xe8\xbe\xb9",
                      "[select] 选项一文字=去东边");
                check(cn.options[0].target == "*east", "[select] 选项一目标=*east");
                check(cn.options[1].target == "*west", "[select] 选项二目标=*west");
            }
        }
        KagRuntime rtSel;
        check(rtSel.load(scSel, &err), "载入 [select] 剧本");
        // 首个停顿是前置旁白「选一个：」，前进到 [select] 生成的 Choice。
        Step selStep = rtSel.advance();
        check(selStep.type == StepType::Choice && selStep.options.size() == 2,
              "[select] 运行时停在双选项");
        Step east = rtSel.choose(0);
        check((east.type == StepType::Say || east.type == StepType::Narr) &&
              east.text.find("\xe4\xb8\x9c") != std::string::npos,
              "[select] 选第一项跳到 *east 结局");
    }

    // [macro] / [call] / 表达式求值
    const char* kMacro =
        "*start\n"
        "[macro name=hello]\n"
        "[eval exp=\"f.called=1\"]\n"
        "【旁白】宏里的句子。[l]\n"
        "[endmacro]\n"
        "[call name=hello]\n"
        "[if exp=\"f.called==1\"][jump target=*ok][endif]\n"
        "没执行宏。[l][s]\n"
        "*ok\n\xe5\xae\x8f\xe5\xb7\xb2\xe6\x89\xa7\xe8\xa1\x8c。[l][s]\n";
    {
        KagScript scM = parseKag(kMacro);
        bool sawDef = false, sawCall = false;
        for (const auto& nd : scM.nodes) {
            if (nd.type == KagType::MacroDef) sawDef = true;
            if (nd.type == KagType::Call && nd.macroName == "hello") sawCall = true;
        }
        check(sawDef, "[macro] 解析为 MacroDef 节点");
        check(sawCall, "[call name=hello] 解析为 Call 且宏名正确");
        KagRuntime rtM;
        check(rtM.load(scM, &err), "载入宏剧本");
        Step m0 = rtM.current();
        check(m0.type == StepType::Say && m0.text.find("\xe5\xae\x8f") != std::string::npos,
              "运行时执行宏体（走到宏里的句子）");
        int gM = 0; bool reachedOk = false;
        Step m = m0;
        while (++gM < 50) {
            // 宏结局行是旁白（无【】标记），故接受 Say/Narr 两种文本停顿。
            if (m.type == StepType::End) break;
            if ((m.type == StepType::Say || m.type == StepType::Narr) &&
                m.text.find("\xe5\xae\x8f\xe5\xb7\xb2") != std::string::npos)
                reachedOk = true;
            m = rtM.advance();
        }
        check(reachedOk, "宏内 [eval] 生效，条件分支跳到 *ok");
    }

    // 跨文件 [jump storage=]（通过 ScenarioLoader 喂入另一份 .ks）
    const char* kCross = "*start\n[jump storage=\"other.ks\" target=*o]\n";
    {
        KagRuntime rtX;
        rtX.setScenarioLoader([](const std::string& storage) -> std::string {
            if (storage == "other.ks")
                return "*o\n【其他】\xe8\xb7\xa8\xe6\x96\x87\xe4\xbb\xb6\xe5\x89\xa7\xe6\x9c\xac\xe5\xb7\xb2\xe5\x8a\xa0\xe8\xbd\xbd。[l][s]\n";
            return std::string();
        });
        check(rtX.load(parseKag(kCross), &err), "载入跨文件剧本（带 loader）");
        Step x = rtX.current();
        check(x.type == StepType::Say &&
              x.text.find("\xe8\xb7\xa8\xe6\x96\x87\xe4\xbb\xb6") != std::string::npos,
              "跨文件 [jump storage=] 加载并跳到目标文件标签");
    }

    std::cout << "RESULT " << g_pass << "/" << (g_pass + g_fail) << "\n";
    return g_fail == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--xp3") == 0) return cmdXP3(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "--kag") == 0) return cmdKag(argv[2]);
    if (argc >= 2 && std::strcmp(argv[1], "--selftest") == 0) return cmdSelfTest();
    std::cerr << "usage: engine_cli --xp3 <file> | --kag <file> | --selftest\n";
    return 2;
}
