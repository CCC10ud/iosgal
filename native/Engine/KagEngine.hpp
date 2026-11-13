// GalBox Native Engine - KAG (.ks) script parser + runtime state machine
// A documented, auditable subset of Kirikiri KAG. It runs real .ks files and
// now covers a much wider tag set (inline formatting, [select], [macro]/[call],
// [if]/[else], expressions, cross-file [jump storage=], audio/trans stubs).
// It still does NOT implement TJS2, complex transitions, audio/video decode or
// commercial XP3 encryption plugins. See compare.html / native.html.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace galbox {

enum class KagType {
    Say, Narr, Label, Choice, Jump, CondJump, Bg, Image,
    Wait, PageClear, ClearText, SetVar, End, Noop, Unknown,
    Select, MacroDef, Call, Return, Erase, ResetVars, Meta
};

// Inline text formatting runs (KAG [font]/[color]/[ruby]/[size]/[b]/[i]/[u]).
// The engine keeps these so a real UI can render rich text; the plain visible
// text is always available as KagNode::text.
enum class RunType { Text, Font, Color, Ruby, Size, Bold, Italic, Underline, Other };
struct KagRun {
    RunType type = RunType::Text;
    std::string text;   // visible characters for this run
    std::string attr;   // tag attribute payload (e.g. color value)
};

struct KagOption {
    std::string text;
    std::string target;
};

struct KagNode {
    KagType type = KagType::Noop;
    std::string who;
    std::string text;       // dialogue / narration / raw unknown tag
    std::vector<KagRun> runs; // rich-text runs for dialogue nodes
    std::string storage;    // bg / image file
    std::string layer;      // image layer
    std::string target;     // jump / true branch (label name)
    std::string callStorage;// [jump]/[call] cross-file storage
    std::string elseTarget; // false branch (CondJump)
    std::string expr;       // condition / assignment expression
    std::string varName;
    long long varValue = 0;
    int cmpOp = 0;          // 0 none, 1 ==, 2 !=, 3 >=, 4 <=, 5 >, 6 <
    long long cmpValue = 0;
    // Macro support: MacroDef skips to jumpTarget; Call to a macro jumps there.
    int jumpTarget = -1;    // node index (macro body start / macro skip)
    std::string macroName;  // [call name=...] target macro
    bool isCall = false;
    std::vector<KagOption> options;
    int line = 0;
    bool visited = false;
};

struct KagScript {
    std::vector<KagNode> nodes;
    std::unordered_map<std::string, int> labels;
    std::vector<std::string> warnings;
};

KagScript parseKag(const std::string& text);

enum class StepType { Say, Narr, Choice, Bg, Image, End, Error };

struct StepOption {
    std::string text;
    int index = 0;
};

struct Step {
    StepType type = StepType::Error;
    std::string who;
    std::string text;
    std::string storage;      // pending background change
    std::string imageStorage; // pending character image
    std::string layer;        // character layer id
    std::string message;
    bool visited = false;
    bool clearPage = false;
    std::vector<StepOption> options;
    int pc = -1;
};

struct HistoryItem {
    int pc = -1;
    std::string who;
    std::string text;
};

// Fetches the text of another scenario file (.ks) by storage name. Returns
// empty string if not found. Enables cross-file [jump storage=...]/[call].
using ScenarioLoader = std::function<std::string(const std::string&)>;

class KagRuntime {
public:
    bool load(const KagScript& script, std::string* err);
    bool loadText(const std::string& text, std::string* err);

    const Step& current() const { return current_; }
    Step advance();
    Step choose(int index);
    Step jumpToLabel(const std::string& label);
    bool jumpToHistory(int historyIndex);

    bool save(std::string& out) const;
    bool restore(const std::string& data, std::string* err);

    // Append a scenario file loaded from the archive (cross-file scripts).
    bool addScenario(const std::string& storage, const std::string& text,
                     std::string* err);
    void setScenarioLoader(ScenarioLoader f) { loader_ = std::move(f); }
    const std::vector<HistoryItem>& history() const { return history_; }
    const std::map<std::string, long long>& vars() const { return vars_; }
    const KagScript& script() const { return script_; }
    std::vector<std::string>& warnings() { return warnings_; }

    // Codec helpers; also used by the free hfield() in the .cpp.
    static std::string hex(const std::string& s);
    static std::string unhex(const std::string& s, bool* ok);

private:
    Step settle();                       // execute non-stopping nodes
    Step next(bool doRecord);            // settle + optionally append history
    void record(const Step& s);
    void ensureVisited();
    bool evalCond(const KagNode& n) const;
    int resolveLabel(const std::string& label, const std::string& storage,
                     std::string* err);

    KagScript script_;
    Step current_;
    std::vector<HistoryItem> history_;
    std::map<std::string, long long> vars_;
    std::vector<std::string> warnings_;
    std::vector<char> visited_;
    std::vector<int> callStack_;
    std::set<std::string> loadedStorages_;   // scenario files already merged
    ScenarioLoader loader_;
    int pc_ = 0;
};

}  // namespace galbox
