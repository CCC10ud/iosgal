#include "KagEngine.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#include <sstream>

namespace galbox {

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}
static std::string stripVarPrefix(std::string s) {
    s = trim(s);
    if (s.compare(0, 3, "sf.") == 0) return s.substr(3);
    if (s.compare(0, 2, "f.") == 0) return s.substr(2);
    return s;
}

// Parse "[bg storage=\"a.png\" time=1000]" -> name + attributes.
static void parseTag(const std::string& body, std::string& name,
                     std::map<std::string, std::string>& attrs) {
    std::string t = trim(body);
    size_t i = 0;
    while (i < t.size() && !std::isspace((unsigned char)t[i])) ++i;
    name = t.substr(0, i);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    while (i < t.size()) {
        while (i < t.size() && std::isspace((unsigned char)t[i])) ++i;
        size_t ks = i;
        while (i < t.size() && t[i] != '=' && !std::isspace((unsigned char)t[i])) ++i;
        std::string key = t.substr(ks, i - ks);
        while (i < t.size() && std::isspace((unsigned char)t[i])) ++i;
        if (i >= t.size() || t[i] != '=') { if (key.empty()) break; continue; }
        ++i;
        while (i < t.size() && std::isspace((unsigned char)t[i])) ++i;
        std::string val;
        if (i < t.size() && (t[i] == '"' || t[i] == '\'')) {
            char q = t[i++];
            while (i < t.size() && t[i] != q) val.push_back(t[i++]);
            if (i < t.size()) ++i;
        } else {
            while (i < t.size() && !std::isspace((unsigned char)t[i])) val.push_back(t[i++]);
        }
        if (!key.empty()) attrs[key] = val;
    }
}

static void splitDialogue(const std::string& raw, KagNode& n) {
    n.type = KagType::Narr;
    n.who.clear();
    n.text = raw;
    if (raw.compare(0, 3, "\xe3\x80\x90") == 0) {  // 【name】
        size_t e = raw.find("\xe3\x80\x91", 3);
        if (e != std::string::npos) {
            n.who = raw.substr(3, e - 3);
            n.text = raw.substr(e + 3);
            n.type = KagType::Say;
            return;
        }
    }
    size_t open = raw.find("\xe3\x80\x8c");        // 「text」
    size_t close = raw.find("\xe3\x80\x8d");
    if (open != std::string::npos && close != std::string::npos && close > open) {
        n.who = trim(raw.substr(0, open));
        n.text = raw.substr(open + 3, close - open - 3);
        n.type = KagType::Say;
    }
}

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// KAG command tags that have explicit semantics in this engine.
static bool isKagCommand(const std::string& n) {
    static const char* k[] = {
        "bg", "image", "fg", "jump", "if", "endif", "else", "eval", "link",
        "endlink", "select", "endselect", "macro", "endmacro", "call", "return",
        "er", "reset", "resetvars", "wait", "waittime", "l", "r", "p", "cm",
        "ct", "s", "end", "playbgm", "stopbgm", "fadeinbgm", "fadeoutbgm",
        "fadein", "fadeout", "trans", "iscript", "endscript", nullptr};
    for (int i = 0; k[i]; ++i) if (n == k[i]) return true;
    return false;
}
// Inline formatting tags become rich-text runs rather than commands.
static bool isInlineTag(const std::string& n) {
    static const char* k[] = {"font", "color", "ruby", "rb", "size", "b", "i",
                              "u", "c", nullptr};
    for (int i = 0; k[i]; ++i) if (n == k[i]) return true;
    return false;
}
static RunType inlineRunType(const std::string& n) {
    if (n == "font" || n == "size") return RunType::Size;
    if (n == "color" || n == "c") return RunType::Color;
    if (n == "ruby" || n == "rb") return RunType::Ruby;
    if (n == "b") return RunType::Bold;
    if (n == "i") return RunType::Italic;
    if (n == "u") return RunType::Underline;
    return RunType::Other;
}

// ---- TJS-like expression evaluator (numbers + comparisons + &&/||) ----
static bool isIdentChar(char c) {
    return std::isalnum((unsigned char)c) || c == '_' || c == '.';
}
static bool evalExpr(const std::string& expIn,
                     const std::map<std::string, long long>& vars,
                     long long& out, std::string* err) {
    std::string exp = trim(expIn);
    if (exp.empty()) { if (err) *err = "空表达式"; return false; }
    struct Tok { enum T { Num, Var, Op, LP, RP, End } t; long long v = 0; std::string s; };
    std::vector<Tok> toks;
    size_t i = 0;
    auto errOut = [&](const std::string& m) { if (err) *err = m; return false; };
    while (i < exp.size()) {
        char c = exp[i];
        if (std::isspace((unsigned char)c)) { ++i; continue; }
        if (c == '(') { toks.push_back({Tok::LP}); ++i; continue; }
        if (c == ')') { toks.push_back({Tok::RP}); ++i; continue; }
        if (c == '&' && i + 1 < exp.size() && exp[i + 1] == '&') { toks.push_back({Tok::Op, 0, "&&"}); i += 2; continue; }
        if (c == '|' && i + 1 < exp.size() && exp[i + 1] == '|') { toks.push_back({Tok::Op, 0, "||"}); i += 2; continue; }
        if (c == '=' && i + 1 < exp.size() && exp[i + 1] == '=') { toks.push_back({Tok::Op, 0, "=="}); i += 2; continue; }
        if (c == '!' && i + 1 < exp.size() && exp[i + 1] == '=') { toks.push_back({Tok::Op, 0, "!="}); i += 2; continue; }
        if (c == '<' && i + 1 < exp.size() && exp[i + 1] == '=') { toks.push_back({Tok::Op, 0, "<="}); i += 2; continue; }
        if (c == '>' && i + 1 < exp.size() && exp[i + 1] == '=') { toks.push_back({Tok::Op, 0, ">="}); i += 2; continue; }
        if (c == '<' || c == '>' || c == '=' || c == '+' || c == '-' ||
            c == '*' || c == '/' || c == '%' || c == '!') {
            toks.push_back({Tok::Op, 0, std::string(1, c)}); ++i; continue;
        }
        if (std::isdigit((unsigned char)c)) {
            size_t j = i; while (j < exp.size() && std::isdigit((unsigned char)exp[j])) ++j;
            toks.push_back({Tok::Num, std::strtoll(exp.substr(i, j - i).c_str(), nullptr, 10)});
            i = j; continue;
        }
        if (std::isalpha((unsigned char)c) || c == '_') {
            size_t j = i; while (j < exp.size() && isIdentChar(exp[j])) ++j;
            toks.push_back({Tok::Var, 0, exp.substr(i, j - i)}); i = j; continue;
        }
        return errOut("无法解析的字符: " + std::string(1, c));
    }
    toks.push_back({Tok::End});
    // Shunting-yard -> RPN.
    std::vector<Tok> rpn; std::vector<Tok> opstack;
    auto prec = [](const std::string& o) -> int {
        if (o == "||") return 1; if (o == "&&") return 2;
        if (o == "==" || o == "!=" || o == "<" || o == ">" || o == "<=" || o == ">=") return 3;
        if (o == "+" || o == "-") return 4; if (o == "*" || o == "/" || o == "%") return 5;
        return 0;
    };
    for (auto& t : toks) {
        if (t.t == Tok::Num || t.t == Tok::Var) rpn.push_back(t);
        else if (t.t == Tok::LP) opstack.push_back(t);
        else if (t.t == Tok::RP) {
            while (!opstack.empty() && opstack.back().t != Tok::LP) { rpn.push_back(opstack.back()); opstack.pop_back(); }
            if (opstack.empty()) return errOut("括号不匹配");
            opstack.pop_back();
        } else if (t.t == Tok::Op) {
            // treat unary - / ! (when previous token is Op/LP/End)
            bool unary = (t.s == "-" || t.s == "!");
            if (unary) {
                size_t k = &t - &toks[0];
                bool first = (k == 0);
                auto prev = [&]() -> Tok::T {
                    if (k == 0) return Tok::End;
                    Tok::T p = toks[k - 1].t;
                    // skip nothing; previous actual token
                    return p;
                };
                if (first || prev() == Tok::Op || prev() == Tok::LP) {
                    std::string u = (t.s == "-") ? "u-" : "u!";
                    rpn.push_back({Tok::Op, 0, u});
                    continue;
                }
            }
            while (!opstack.empty() && opstack.back().t == Tok::Op &&
                   prec(opstack.back().s) >= prec(t.s)) {
                rpn.push_back(opstack.back()); opstack.pop_back();
            }
            opstack.push_back(t);
        } else { /* End */ while (!opstack.empty()) { rpn.push_back(opstack.back()); opstack.pop_back(); } }
    }
    // Evaluate RPN.
    std::vector<long long> st;
    for (auto& t : rpn) {
        if (t.t == Tok::Num) st.push_back(t.v);
        else if (t.t == Tok::Var) st.push_back(vars.count(stripVarPrefix(t.s)) ? vars.at(stripVarPrefix(t.s)) : 0);
        else if (t.t == Tok::Op) {
            if (t.s == "u-") { if (st.empty()) return errOut("表达式不完整"); long long a = st.back(); st.back() = -a; continue; }
            if (t.s == "u!") { if (st.empty()) return errOut("表达式不完整"); st.back() = !st.back(); continue; }
            if (st.size() < 2) return errOut("表达式不完整");
            long long b = st.back(); st.pop_back();
            long long a = st.back(); st.back() = [a, b, &t]() -> long long {
                if (t.s == "+") return a + b;
                if (t.s == "-") return a - b;
                if (t.s == "*") return a * b;
                if (t.s == "/") return b == 0 ? 0 : a / b;
                if (t.s == "%") return b == 0 ? 0 : a % b;
                if (t.s == "==") return a == b;
                if (t.s == "!=") return a != b;
                if (t.s == "<") return a < b;
                if (t.s == ">") return a > b;
                if (t.s == "<=") return a <= b;
                if (t.s == ">=") return a >= b;
                if (t.s == "&&") return a && b;
                if (t.s == "||") return a || b;
                return 0;
            }();
        }
    }
    if (st.size() != 1) return errOut("表达式求值失败");
    out = st[0];
    return true;
}

static bool evalAssign(const std::string& expIn,
                       std::map<std::string, long long>& vars, std::string& warn) {
    std::string exp = trim(expIn);
    size_t eq = exp.find('=');
    // Find the assignment operator among += -= *= /= = (first '=' not part of == !=)
    size_t pos = std::string::npos;
    for (size_t k = 0; k + 1 < exp.size(); ++k) {
        if (exp[k] == '=' && exp[k + 1] != '=') { pos = k; break; }
    }
    if (pos == std::string::npos) { warn = "赋值表达式缺少 '='"; return false; }
    bool compound = pos > 0 && (exp[pos - 1] == '+' || exp[pos - 1] == '-' ||
                                exp[pos - 1] == '*' || exp[pos - 1] == '/');
    std::string lhs = trim(exp.substr(0, compound ? pos - 1 : pos));
    std::string rhs = trim(exp.substr(pos + 1));
    lhs = stripVarPrefix(lhs);
    if (lhs.empty()) { warn = "赋值左侧为空"; return false; }
    long long rv = 0;
    bool isStr = (rhs.size() >= 2 && (rhs.front() == '"' || rhs.front() == '\''));
    if (isStr) { warn = "字符串赋值在子集引擎中暂存为 0: " + rhs; rv = 0; }
    else {
        std::string e;
        if (!evalExpr(rhs, vars, rv, &e)) { warn = "右侧表达式错误: " + e; return false; }
    }
    if (!compound) vars[lhs] = rv;
    else {
        char op = exp[pos - 1];
        long long base = vars.count(lhs) ? vars[lhs] : 0;
        if (op == '+') vars[lhs] = base + rv;
        else if (op == '-') vars[lhs] = base - rv;
        else if (op == '*') vars[lhs] = base * rv;
        else if (op == '/') vars[lhs] = (rv == 0) ? 0 : base / rv;
    }
    return true;
}

// Records a macro body-start label so [call name=...] can resolve at runtime.
static void macroTargetsNote(KagScript& sc, const std::string& name, int idx) {
    sc.labels["__macro__" + name] = idx;
}

// --------------------------- parser ---------------------------
KagScript parseKag(const std::string& text) {
    KagScript out;
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;

    auto push = [&](const KagNode& n) {
        out.nodes.push_back(n);
        out.nodes.back().line = lineNo;
    };
    // Finalize accumulated dialogue runs into a Say/Narr node.
    auto finalize = [&](std::vector<KagRun>& cur) -> bool {
        std::string vis;
        for (const auto& r : cur) vis += r.text;
        if (trim(vis).empty()) { cur.clear(); return false; }
        KagNode n;
        splitDialogue(vis, n);
        n.runs = cur;
        cur.clear();
        push(n);
        return true;
    };

    std::vector<KagRun> cur;          // current dialogue accumulator
    bool linkMode = false;
    std::string linkTarget, linkText;
    int selectOpen = -1;              // node index of the open Choice (select)
    bool inScript = false;            // inside [iscript]...[endscript]
    std::vector<std::pair<std::string, int>> macroStack; // name -> MacroDef node idx

    auto flush = [&]() { finalize(cur); };

    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string s = trim(line);

        // [iscript] blocks are TJS; skip them entirely (engine has no TJS2).
        if (inScript) {
            if (s.find("[endscript]") != std::string::npos) inScript = false;
            continue;
        }
        if (s.empty() || s[0] == ';') continue;
        if (s[0] == '*') {
            flush();
            KagNode n;
            n.type = KagType::Label;
            std::string rest = s.substr(1);
            size_t bar = rest.find('|');
            n.text = (bar == std::string::npos) ? trim(rest) : trim(rest.substr(0, bar));
            out.labels[n.text] = (int)out.nodes.size();
            push(n);
            continue;
        }
        if (s.find("[iscript]") != std::string::npos) { inScript = true; continue; }

        // Scan the line: text spans + tag spans.
        size_t i = 0;
        while (i < s.size()) {
            if (s[i] != '[') {
                // text character (also handles 」「 name markers later)
                if (linkMode) linkText.push_back(s[i]);
                else {
                    if (cur.empty() || cur.back().type != RunType::Text)
                        cur.push_back({RunType::Text, std::string()});
                    cur.back().text.push_back(s[i]);
                }
                ++i;
                continue;
            }
            size_t e = s.find(']', i);
            if (e == std::string::npos) {
                if (linkMode) linkText.append(s.substr(i)); else {
                    if (cur.empty() || cur.back().type != RunType::Text)
                        cur.push_back({RunType::Text, std::string()});
                    cur.back().text.append(s.substr(i));
                }
                break;
            }
            std::string body = s.substr(i + 1, e - i - 1);
            i = e + 1;
            std::string tag; std::map<std::string, std::string> a;
            parseTag(body, tag, a);
            std::string lower = tag; std::transform(lower.begin(), lower.end(), lower.begin(),
                                                   [](unsigned char c){return (char)std::tolower(c);});

            // Option inside a [select] block: [*text|*target]
            if (selectOpen >= 0 && !lower.empty() && lower[0] == '*') {
                std::string inner = body.substr(1);
                size_t bar = inner.find('|');
                std::string ot = (bar == std::string::npos) ? trim(inner) : trim(inner.substr(0, bar));
                std::string otarget = (bar == std::string::npos) ? "" : trim(inner.substr(bar + 1));
                if (otarget.empty()) out.warnings.push_back("第 " + std::to_string(lineNo) + " 行 [select] 选项缺少目标");
                out.nodes[selectOpen].options.push_back({ot, otarget});
                continue;
            }

            if (linkMode && lower != "endlink") {
                if (lower == "r") linkText.push_back('\n');
                else linkText.append("[").append(body).append("]");
                continue;
            }

            if (lower == "endlink" && linkMode) {
                KagNode n; n.type = KagType::Choice;
                n.options.push_back({trim(linkText), linkTarget});
                push(n);
                linkMode = false; linkText.clear(); linkTarget.clear();
                continue;
            }
            if (lower == "link") {
                flush();
                linkMode = true; linkText.clear();
                linkTarget = a.count("target") ? a["target"] : "";
                continue;
            }

            if (isKagCommand(lower)) {
                // Some commands flush the pending dialogue first.
                if (lower == "r") {
                    if (cur.empty() || cur.back().type != RunType::Text)
                        cur.push_back({RunType::Text, std::string()});
                    cur.back().text.push_back('\n');
                    continue;
                }
                flush();
                KagNode n;
                if (lower == "l" || lower == "wait" || lower == "waittime") n.type = KagType::Wait;
                else if (lower == "p" || lower == "cm" || lower == "ct") n.type = KagType::PageClear;
                else if (lower == "s" || lower == "end") { n.type = KagType::End; n.text.clear(); }
                else if (lower == "er") { n.type = KagType::Erase; }
                else if (lower == "reset" || lower == "resetvars") n.type = KagType::ResetVars;
                else if (lower == "bg") { n.type = KagType::Bg; n.storage = a["storage"]; }
                else if (lower == "image" || lower == "fg") { n.type = KagType::Image; n.storage = a["storage"]; n.layer = a.count("layer") ? a["layer"] : "0"; }
                else if (lower == "jump") { n.type = KagType::Jump; n.target = a["target"]; n.callStorage = a.count("storage") ? a["storage"] : ""; }
                else if (lower == "if") { n.type = KagType::CondJump; n.expr = a["exp"]; }
                else if (lower == "endif") { n.type = KagType::Noop; n.text = "__endif__"; }
                else if (lower == "else") { n.type = KagType::Noop; n.text = "__else__"; }
                else if (lower == "eval") { n.type = KagType::SetVar; n.expr = a["exp"]; }
                else if (lower == "select") { n.type = KagType::Choice; selectOpen = (int)out.nodes.size(); }
                else if (lower == "endselect") { selectOpen = -1; continue; }
                else if (lower == "macro") {
                    n.type = KagType::MacroDef;
                    macroStack.push_back({a.count("name") ? a["name"] : "", (int)out.nodes.size()});
                }
                else if (lower == "endmacro") {
                    int retIdx = (int)out.nodes.size();
                    n.type = KagType::Return;
                    if (!macroStack.empty()) {
                        int d = macroStack.back().second;
                        out.nodes[d].jumpTarget = retIdx + 1;       // skip body in normal flow
                        macroTargetsNote(out, macroStack.back().first, d + 1);
                        macroStack.pop_back();
                    } else {
                        out.warnings.push_back("第 " + std::to_string(lineNo) + " 行 [endmacro] 没有配对 [macro]");
                    }
                }
                else if (lower == "call") {
                    n.type = KagType::Call;
                    if (a.count("name")) n.macroName = a["name"];
                    else { n.target = a["target"]; n.callStorage = a.count("storage") ? a["storage"] : ""; }
                }
                else if (lower == "return") n.type = KagType::Return;
                else if (lower == "iscript") { /* handled by line scan; treat as no-op here */ n.type = KagType::Noop; }
                else { n.type = KagType::Meta; n.text = "[" + body + "]"; } // audio/trans stub
                push(n);
                continue;
            }

            if (isInlineTag(lower)) {
                // Inline formatting: try to capture paired [x]...[/x] on same line.
                std::string close = "[/" + lower + "]";
                size_t ce = s.find(close, i);
                std::string inner;
                if (ce != std::string::npos) {
                    inner = s.substr(i, ce - i);
                    i = ce + close.size();
                }
                RunType rt = inlineRunType(lower);
                KagRun run; run.type = rt;
                if (rt == RunType::Color) run.attr = a.count("color") ? a["color"] : a.count("c") ? a["c"] : (a.empty() ? lower : a.begin()->second);
                else if (rt == RunType::Ruby) run.text = a.count("text") ? a["text"] : a.count("t") ? a["t"] : inner;
                else if (rt == RunType::Size) run.attr = a.count("size") ? a["size"] : a.count("s") ? a["s"] : inner;
                else run.text = inner;
                if (linkMode) linkText += inner;
                else cur.push_back(run);
                continue;
            }

            // Unknown tag: keep as text-ish but warn (handled as Unknown at runtime).
            if (linkMode) linkText.append("[").append(body).append("]");
            else {
                if (cur.empty() || cur.back().type != RunType::Text)
                    cur.push_back({RunType::Text, std::string()});
                cur.back().text.append("[").append(body).append("]");
            }
            out.warnings.push_back("第 " + std::to_string(lineNo) + " 行未支持标签: " + tag);
        }
        if (linkMode)
            out.warnings.push_back("第 " + std::to_string(lineNo) + " 行 [link] 没有闭合 [endlink]");
        flush();
    }

    // Second pass: pair [if]/[else]/[endif], merge choices, resolve macro calls.
    std::vector<int> ifStack;
    for (size_t i = 0; i < out.nodes.size(); ++i) {
        KagNode& n = out.nodes[i];
        if (n.type == KagType::CondJump) ifStack.push_back((int)i);
        else if (n.type == KagType::Noop && n.text == "__endif__") {
            if (!ifStack.empty()) {
                out.nodes[ifStack.back()].elseTarget = std::to_string((int)i + 1);
                ifStack.pop_back();
            }
        } else if (n.type == KagType::Noop && n.text == "__else__") {
            if (!ifStack.empty()) {
                out.nodes[ifStack.back()].elseTarget = std::to_string((int)i + 1);
                ifStack.pop_back();
            }
        }
        if (n.type == KagType::Choice) {
            size_t j = i + 1;
            while (j < out.nodes.size() && out.nodes[j].type == KagType::Choice) {
                for (const KagOption& o : out.nodes[j].options) n.options.push_back(o);
                out.nodes[j].type = KagType::Noop;
                ++j;
            }
            if (selectOpen < 0) i = j - 1;
        }
        if (n.type == KagType::Call && !n.macroName.empty()) {
            // resolved by runtime via macroTargets_; nothing to do here
        }
    }
    for (int idx : ifStack)
        out.warnings.push_back("第 " + std::to_string(out.nodes[idx].line) + " 行的 [if] 没有配对 [endif]");
    return out;
}

// ---------------- runtime ----------------
std::string KagRuntime::hex(const std::string& s) {
    static const char* d = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) { o.push_back(d[c >> 4]); o.push_back(d[c & 15]); }
    return o;
}
std::string KagRuntime::unhex(const std::string& s, bool* ok) {
    if (ok) *ok = true;
    if (s == "-") return "";
    if (s.size() % 2) { if (ok) *ok = false; return ""; }
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::string o;
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = nib(s[i]), b = nib(s[i + 1]);
        if (a < 0 || b < 0) { if (ok) *ok = false; return ""; }
        o.push_back((char)((a << 4) | b));
    }
    return o;
}
static std::string hfield(const std::string& s) {
    return s.empty() ? std::string("-") : KagRuntime::hex(s);
}

void KagRuntime::ensureVisited() {
    if (visited_.size() < script_.nodes.size())
        visited_.resize(script_.nodes.size(), 0);
}

bool KagRuntime::evalCond(const KagNode& n) const {
    long long v = 0; std::string e;
    if (evalExpr(n.expr, vars_, v, &e)) return v != 0;
    // Fallback to the simple comparison parsed earlier.
    auto it = vars_.find(n.varName);
    long long x = (it == vars_.end()) ? 0 : it->second;
    switch (n.cmpOp) {
        case 1: return x == n.cmpValue;
        case 2: return x != n.cmpValue;
        case 3: return x >= n.cmpValue;
        case 4: return x <= n.cmpValue;
        case 5: return x > n.cmpValue;
        case 6: return x < n.cmpValue;
        default: return x != 0;
    }
}

int KagRuntime::resolveLabel(const std::string& label, const std::string& storage, std::string* err) {
    std::string t = label;
    if (!t.empty() && t[0] == '*') t.erase(0, 1);
    auto it = script_.labels.find(t);
    if (it != script_.labels.end()) return it->second;
    if (!storage.empty() && loader_) {
        if (loadedStorages_.count(storage) == 0) {
            std::string txt = loader_(storage);
            if (!txt.empty()) {
                std::string e2;
                if (!addScenario(storage, txt, &e2))
                    warnings_.push_back("加载场景失败 " + storage + ": " + e2);
                loadedStorages_.insert(storage);
            }
        }
        it = script_.labels.find(t);
        if (it != script_.labels.end()) return it->second;
    }
    if (err) *err = "标签不存在: " + label + (storage.empty() ? "" : " (storage=" + storage + ")");
    return -1;
}

Step KagRuntime::settle() {
    Step s;
    std::string pendingBg, pendingImg, pendingLayer;
    bool pendingClear = false;
    std::set<int> warned;
    int guard = 0;

    while (++guard < 20000) {
        if (pc_ < 0 || pc_ >= (int)script_.nodes.size()) {
            s.type = StepType::End; s.text = "(剧本结束)"; s.pc = pc_; return s;
        }
        const KagNode& n = script_.nodes[pc_];

        auto fillEffects = [&]() {
            s.storage = pendingBg; s.imageStorage = pendingImg; s.layer = pendingLayer;
            s.clearPage = pendingClear; s.pc = pc_; ensureVisited();
            s.visited = visited_[pc_] != 0;
        };

        switch (n.type) {
            case KagType::Say:
            case KagType::Narr:
            case KagType::Choice:
            case KagType::End:
            case KagType::Erase: {
                StepType st = (n.type == KagType::Say) ? StepType::Say
                           : (n.type == KagType::Narr) ? StepType::Narr
                           : (n.type == KagType::Choice) ? StepType::Choice
                           : StepType::End;
                s.type = st;
                s.who = n.who; s.text = n.text;
                for (const KagOption& o : n.options)
                    s.options.push_back({o.text, (int)s.options.size()});
                fillEffects();
                return s;
            }
            case KagType::Bg: pendingBg = n.storage; ++pc_; break;
            case KagType::Image: pendingImg = n.storage; pendingLayer = n.layer; ++pc_; break;
            case KagType::PageClear:
            case KagType::ClearText: pendingClear = true; ++pc_; break;
            case KagType::ResetVars: vars_.clear(); ++pc_; break;
            case KagType::SetVar: {
                std::string w;
                if (!evalAssign(n.expr, vars_, w)) warnings_.push_back("赋值失败: " + w);
                else if (!w.empty()) warnings_.push_back(w);
                ++pc_; break;
            }
            case KagType::Jump: {
                if (n.callStorage.empty() && (n.target.empty() || n.target[0] != '*')) {
                    s.type = StepType::Error; s.message = "jump 缺少目标"; s.pc = pc_; return s;
                }
                std::string e; int dest = resolveLabel(n.target, n.callStorage, &e);
                if (dest < 0) { s.type = StepType::Error; s.message = e; s.pc = pc_; return s; }
                pc_ = dest; break;
            }
            case KagType::Call: {
                int dest = -1; std::string e;
                if (n.macroName.empty()) {
                    dest = resolveLabel(n.target, n.callStorage, &e);
                } else {
                    auto it = script_.labels.find("__macro__" + n.macroName);
                    if (it != script_.labels.end()) dest = it->second;
                    else { e = "未定义宏: " + n.macroName; }
                }
                if (dest < 0) { s.type = StepType::Error; s.message = e; s.pc = pc_; return s; }
                callStack_.push_back(pc_ + 1);
                pc_ = dest; break;
            }
            case KagType::Return: {
                if (callStack_.empty()) { s.type = StepType::End; s.text = "(返回到剧本开头)"; s.pc = pc_; return s; }
                pc_ = callStack_.back(); callStack_.pop_back(); break;
            }
            case KagType::MacroDef: {
                if (n.jumpTarget >= 0) pc_ = n.jumpTarget; else ++pc_; break;
            }
            case KagType::CondJump: {
                if (evalCond(n)) ++pc_;
                else {
                    int dest = n.elseTarget.empty() ? pc_ + 1 : std::atoi(n.elseTarget.c_str());
                    pc_ = dest;
                }
                break;
            }
            case KagType::Meta: ++pc_; break;  // audio/trans/iscript: silent no-op
            case KagType::Unknown:
                if (warned.insert(pc_).second)
                    warnings_.push_back("跳过未支持标签: " + n.text);
                ++pc_; break;
            default: ++pc_; break;
        }
    }
    s.type = StepType::Error; s.message = "指令循环超过 20000 步，剧本可能存在死循环"; s.pc = pc_;
    return s;
}

void KagRuntime::record(const Step& s) {
    ensureVisited();
    if (s.pc >= 0 && s.pc < (int)visited_.size()) visited_[s.pc] = 1;
    HistoryItem h; h.pc = s.pc; h.who = s.who; h.text = s.text;
    history_.push_back(h);
}

Step KagRuntime::next(bool doRecord) {
    Step s = settle();
    // Erase nodes settle to StepType::End, so End covers them here.
    if (doRecord && (s.type == StepType::Say || s.type == StepType::Narr ||
                     s.type == StepType::Choice || s.type == StepType::End))
        record(s);
    current_ = s;
    return s;
}

bool KagRuntime::load(const KagScript& script, std::string* err) {
    script_ = script;
    warnings_ = script.warnings;
    visited_.assign(script_.nodes.size(), 0);
    history_.clear(); vars_.clear(); callStack_.clear();
    loadedStorages_.clear();
    pc_ = 0;
    if (script_.nodes.empty()) { if (err) *err = "剧本没有任何节点"; return false; }
    Step s = next(true);
    return s.type != StepType::Error || (err ? (*err = s.message, false) : false);
}

bool KagRuntime::loadText(const std::string& text, std::string* err) {
    return load(parseKag(text), err);
}

bool KagRuntime::addScenario(const std::string& storage, const std::string& text, std::string* err) {
    KagScript more = parseKag(text);
    int base = (int)script_.nodes.size();
    for (auto& nd : more.nodes) { nd.line += 100000; script_.nodes.push_back(nd); }
    for (auto& kv : more.labels) script_.labels[kv.first] = kv.second + base;
    // Offset cross-file macro skip targets so normal flow skips the body.
    for (size_t i = base; i < script_.nodes.size(); ++i)
        if (script_.nodes[i].type == KagType::MacroDef && script_.nodes[i].jumpTarget >= 0)
            script_.nodes[i].jumpTarget += base;
    // re-run if/endif + choice merge on the appended region (labels use base offset)
    std::vector<int> ifStack;
    for (size_t i = base; i < script_.nodes.size(); ++i) {
        KagNode& n = script_.nodes[i];
        if (n.type == KagType::CondJump) ifStack.push_back((int)i);
        else if (n.type == KagType::Noop && n.text == "__endif__") {
            if (!ifStack.empty()) { script_.nodes[ifStack.back()].elseTarget = std::to_string((int)i + 1); ifStack.pop_back(); }
        } else if (n.type == KagType::Noop && n.text == "__else__") {
            if (!ifStack.empty()) { script_.nodes[ifStack.back()].elseTarget = std::to_string((int)i + 1); ifStack.pop_back(); }
        }
    }
    ensureVisited();
    return true;
}

Step KagRuntime::advance() {
    if (current_.type == StepType::End || current_.type == StepType::Error)
        return current_;
    pc_ = current_.pc + 1;
    return next(true);
}

Step KagRuntime::choose(int index) {
    if (current_.type != StepType::Choice || index < 0 ||
        index >= (int)current_.options.size()) {
        Step s; s.type = StepType::Error; s.message = "当前不是可选项或选项下标越界"; s.pc = pc_;
        current_ = s; return s;
    }
    const KagNode& n = script_.nodes[current_.pc];
    if (index >= (int)n.options.size()) {
        Step s; s.type = StepType::Error; s.message = "选项数据不一致"; s.pc = pc_;
        current_ = s; return s;
    }
    std::string t = n.options[index].target;
    std::string e; int dest = resolveLabel(t, "", &e);
    if (dest < 0) { Step s; s.type = StepType::Error; s.message = e; s.pc = pc_; current_ = s; return s; }
    pc_ = dest;
    return next(true);
}

Step KagRuntime::jumpToLabel(const std::string& label) {
    std::string e; int dest = resolveLabel(label, "", &e);
    if (dest < 0) { Step s; s.type = StepType::Error; s.message = e; s.pc = pc_; current_ = s; return s; }
    pc_ = dest; history_.clear(); return next(true);
}

bool KagRuntime::jumpToHistory(int i) {
    if (i < 0 || i >= (int)history_.size()) return false;
    pc_ = history_[i].pc; history_.resize(i); next(true); return true;
}

bool KagRuntime::save(std::string& out) const {
    out = "GBSAVE1\n";
    out += "pc " + std::to_string(pc_) + "\n";
    out += "bg " + hfield(current_.storage) + "\n";
    out += "img " + hfield(current_.imageStorage) + " " + hfield(current_.layer) + "\n";
    for (const auto& kv : vars_)
        out += "var " + hfield(kv.first) + " " + std::to_string(kv.second) + "\n";
    for (size_t i = 0; i < visited_.size(); ++i)
        if (visited_[i]) out += "visit " + std::to_string(i) + "\n";
    for (const HistoryItem& h : history_)
        out += "log " + std::to_string(h.pc) + " " + hfield(h.who) + " " + hfield(h.text) + "\n";
    return true;
}

bool KagRuntime::restore(const std::string& data, std::string* err) {
    std::istringstream in(data);
    std::string magic;
    if (!std::getline(in, magic) || magic != "GBSAVE1") {
        if (err) *err = "存档格式不匹配（需要 GBSAVE1）"; return false;
    }
    vars_.clear(); visited_.assign(script_.nodes.size(), 0); history_.clear();
    int restoredPc = 0; bool ok = true;
    std::string savedBg, savedImg, savedLayer; std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line); std::string kind;
        if (!(ls >> kind)) continue;
        if (kind == "pc") ls >> restoredPc;
        else if (kind == "bg") { std::string v; ls >> v; bool g = false; savedBg = unhex(v, &g); if (!g) { ok = false; break; } }
        else if (kind == "img") { std::string a, b; ls >> a >> b; bool g1 = false, g2 = false;
            savedImg = unhex(a, &g1); savedLayer = unhex(b, &g2); if (!g1 || !g2) { ok = false; break; } }
        else if (kind == "var") { std::string k, v; ls >> k >> v; bool good = false;
            std::string key = unhex(k, &good); if (!good) { ok = false; break; }
            vars_[key] = std::strtoll(v.c_str(), nullptr, 10); }
        else if (kind == "visit") { int idx = -1; ls >> idx; if (idx >= 0 && idx < (int)visited_.size()) visited_[idx] = 1; }
        else if (kind == "log") { int p = -1; std::string w, t; ls >> p >> w >> t; bool g1 = false, g2 = false;
            HistoryItem h; h.pc = p; h.who = unhex(w, &g1); h.text = unhex(t, &g2);
            if (!g1 || !g2) { ok = false; break; } history_.push_back(h); }
    }
    if (!ok) { if (err) *err = "存档内容损坏（十六进制字段无法解码）"; return false; }
    pc_ = restoredPc; next(false);
    current_.storage = savedBg; current_.imageStorage = savedImg; current_.layer = savedLayer;
    return true;
}

}  // namespace galbox
