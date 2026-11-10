// M2.0 CardSpec core tests: strict v1 inline-only schema, typed validation,
// deterministic serialization and the error matrix. No window, file, network
// or executable capability is touched here.
#include "data/cardspec/cardspec.h"

#include "data/assistant/json.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void Check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("[cardspec-test] FAIL %s\n", what.c_str());
        ++failures;
    }
}

std::string U8(std::wstring_view text) { return assistant::ToUtf8(text); }

std::string Repeat(char c, std::size_t count) { return std::string(count, c); }

// One non-BMP code point as a UTF-16 surrogate pair (two wchar_t units on
// Windows); avoids astral universal-character-names in wide literals.
std::wstring Emoji(std::size_t count) {
    std::wstring out;
    out.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(static_cast<wchar_t>(0xD83D));
        out.push_back(static_cast<wchar_t>(0xDE00));
    }
    return out;
}

// Raw JSON string literal: callers supply the contents including any escapes
// or invalid bytes they want to exercise.
std::string Q(std::string_view content) { return "\"" + std::string(content) + "\""; }

std::string Obj(std::initializer_list<std::pair<std::string, std::string>> members) {
    std::string out = "{";
    bool first = true;
    for (const auto& member : members) {
        if (!first) out += ",";
        first = false;
        out += Q(member.first);
        out += ":";
        out += member.second;
    }
    out += "}";
    return out;
}

std::string Arr(std::initializer_list<std::string> items) {
    std::string out = "[";
    bool first = true;
    for (const auto& item : items) {
        if (!first) out += ",";
        first = false;
        out += item;
    }
    out += "]";
    return out;
}

std::string Inline() { return "{\"type\":\"inline\"}"; }

std::string RowsItem(const std::string& label, const std::string& text) {
    return Obj({{"label", label}, {"text", text}});
}

std::string BarItem(const std::string& label, const std::string& value,
                    const std::string& max) {
    return Obj({{"label", label}, {"value", value}, {"max", max}});
}

std::string RootDoc(const std::string& version, const std::string& title,
                    const std::string& source, const std::string& layout,
                    const std::string& rows) {
    return Obj({{"version", version},
                {"title", title},
                {"source", source},
                {"layout", layout},
                {"rows", rows}});
}

std::string GoodRowsDoc() {
    return RootDoc("1", Q(U8(L"今日目标")), Inline(), Q("rows"),
                   Arr({RowsItem(Q(U8(L"学习")), Q(U8(L"阅读三十分钟")))}));
}

std::string GoodBarDoc() {
    return RootDoc("1", Q(U8(L"目标进度")), Inline(), Q("bar"),
                   Arr({BarItem(Q(U8(L"学习")), "3", "5")}));
}

bool SameSpec(const cardspec::Spec& a, const cardspec::Spec& b) {
    if (a.title != b.title || a.layout != b.layout || a.rows.size() != b.rows.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.rows.size(); ++i) {
        if (a.rows[i].label != b.rows[i].label) return false;
        if (a.rows[i].text != b.rows[i].text) return false;
        if (a.rows[i].value != b.rows[i].value) return false;
        if (a.rows[i].max != b.rows[i].max) return false;
    }
    return true;
}

bool ParseOk(const std::string& label, const std::string& text, cardspec::Spec* out = nullptr) {
    cardspec::Spec local;
    cardspec::Spec* target = out ? out : &local;
    std::wstring error = L"stale error";
    const bool ok = cardspec::Parse(text, target, &error);
    Check(ok, label + ": parses");
    if (ok) Check(error.empty(), label + ": success clears error");
    return ok;
}

// Rejection checks: *out untouched, error set, no payload echo, optional
// fragment (field path / limit) present.
void ParseReject(const std::string& label, const std::string& text,
                 const std::wstring& fragment = L"") {
    cardspec::Spec sentinel;
    sentinel.title = L"OUT_SENTINEL";
    sentinel.layout = cardspec::Layout::Bar;
    cardspec::Row row;
    row.label = L"keep";
    row.value = 1.0;
    row.max = 2.0;
    sentinel.rows.push_back(row);

    std::wstring error = L"stale error";
    const bool ok = cardspec::Parse(text, &sentinel, &error);
    Check(!ok, label + ": rejected");
    if (ok) return;
    Check(!error.empty(), label + ": error message is set");
    Check(error != L"stale error", label + ": error replaces stale error");
    Check(error.find(L"OUT_SENTINEL") == std::wstring::npos,
          label + ": error does not echo payload");
    Check(error.find(L"SECRET9f3") == std::wstring::npos,
          label + ": error does not echo secret sentinel");
    Check(sentinel.title == L"OUT_SENTINEL" && sentinel.layout == cardspec::Layout::Bar &&
              sentinel.rows.size() == 1 && sentinel.rows[0].label == L"keep" &&
              sentinel.rows[0].value == 1.0 && sentinel.rows[0].max == 2.0,
          label + ": failure leaves out untouched");
    if (!fragment.empty()) {
        Check(error.find(fragment) != std::wstring::npos,
              label + ": error mentions " + assistant::ToUtf8(fragment));
    }
}

void ExpectValidateFail(const std::string& label, const cardspec::Spec& spec,
                        const std::wstring& fragment = L"") {
    std::wstring error = L"stale error";
    Check(!cardspec::Validate(spec, &error), label + ": typed input rejected");
    Check(!error.empty(), label + ": typed error message is set");
    if (!fragment.empty()) {
        Check(error.find(fragment) != std::wstring::npos,
              label + ": typed error mentions " + assistant::ToUtf8(fragment));
    }
    Check(cardspec::Serialize(spec).empty(), label + ": invalid typed input serializes to empty");
}

cardspec::Spec MakeRowsSpec() {
    cardspec::Spec spec;
    spec.title = L"今日目标";
    spec.layout = cardspec::Layout::Rows;
    cardspec::Row row;
    row.label = L"学习";
    row.text = L"阅读三十分钟";
    spec.rows.push_back(row);
    return spec;
}

cardspec::Spec MakeBarSpec() {
    cardspec::Spec spec;
    spec.title = L"目标进度";
    spec.layout = cardspec::Layout::Bar;
    cardspec::Row row;
    row.label = L"学习";
    row.value = 3.0;
    row.max = 5.0;
    spec.rows.push_back(row);
    return spec;
}

// --- Legal documents ----------------------------------------------------

void TestLegalDocuments() {
    cardspec::Spec rows;
    if (ParseOk("rows layout", GoodRowsDoc(), &rows)) {
        Check(rows.title == L"今日目标", "rows title decoded");
        Check(rows.layout == cardspec::Layout::Rows, "rows layout decoded");
        Check(rows.rows.size() == 1, "rows count");
        Check(rows.rows[0].label == L"学习", "rows label decoded");
        Check(rows.rows[0].text == L"阅读三十分钟", "rows text decoded");
    }

    cardspec::Spec bar;
    if (ParseOk("bar layout", GoodBarDoc(), &bar)) {
        Check(bar.layout == cardspec::Layout::Bar, "bar layout decoded");
        Check(bar.rows.size() == 1 && bar.rows[0].value == 3.0 && bar.rows[0].max == 5.0,
              "bar numbers decoded");
    }

    ParseOk("leading BOM", std::string("\xEF\xBB\xBF") + GoodRowsDoc());
    ParseOk("surrounding whitespace", "\r\n\t " + GoodRowsDoc() + " \n");
    ParseOk("fractional version 1.0", RootDoc("1.0", Q("t"), Inline(), Q("rows"),
                                             Arr({RowsItem(Q("l"), Q("t"))})));
    ParseOk("exponent version 1e0", RootDoc("1e0", Q("t"), Inline(), Q("rows"),
                                            Arr({RowsItem(Q("l"), Q("t"))})));

    // UTF-8 text, \\u escapes and surrogate pairs.
    cardspec::Spec escaped;
    if (ParseOk("unicode escapes", RootDoc("1", Q("\\u4eca\\u65e5"), Inline(), Q("rows"),
                                           Arr({RowsItem(Q("l"), Q("t"))})),
                &escaped)) {
        Check(escaped.title == L"今日", "unicode escape decoded");
    }
    cardspec::Spec emoji;
    if (ParseOk("surrogate pair escape",
                RootDoc("1", Q("\\ud83d\\ude00"), Inline(), Q("rows"),
                        Arr({RowsItem(Q("l"), Q("t"))})),
                &emoji)) {
        Check(emoji.title == Emoji(1), "surrogate pair decoded");
        Check(emoji.title.size() == 2, "surrogate pair uses two UTF-16 units");
    }

    // Boundary lengths: title 64 units, label 32, text 96, 8 rows, 1 row.
    ParseOk("title 64 units", RootDoc("1", Q(Repeat('a', 64)), Inline(), Q("rows"),
                                      Arr({RowsItem(Q("l"), Q("t"))})));
    ParseOk("title 64 units from 32 emoji",
            RootDoc("1", Q(U8(Emoji(32))), Inline(), Q("rows"),
                    Arr({RowsItem(Q("l"), Q("t"))})));
    ParseOk("label 32 / text 96 units",
            RootDoc("1", Q("t"), Inline(), Q("rows"),
                    Arr({RowsItem(Q(Repeat('l', 32)), Q(Repeat('t', 96)))})));
    ParseOk("eight rows", RootDoc("1", Q("t"), Inline(), Q("rows"),
                                  Arr({RowsItem(Q("a"), Q("b")), RowsItem(Q("a"), Q("b")),
                                       RowsItem(Q("a"), Q("b")), RowsItem(Q("a"), Q("b")),
                                       RowsItem(Q("a"), Q("b")), RowsItem(Q("a"), Q("b")),
                                       RowsItem(Q("a"), Q("b")), RowsItem(Q("a"), Q("b"))})));
    ParseOk("bar value zero", RootDoc("1", Q("t"), Inline(), Q("bar"),
                                      Arr({BarItem(Q("l"), "0", "5")})));
    ParseOk("bar value equals max", RootDoc("1", Q("t"), Inline(), Q("bar"),
                                            Arr({BarItem(Q("l"), "5", "5")})));
    ParseOk("bar fractional", RootDoc("1", Q("t"), Inline(), Q("bar"),
                                      Arr({BarItem(Q("l"), "0.5", "2.5")})));

    // Size limit is inclusive and the BOM counts towards it.
    std::string padded = GoodRowsDoc();
    padded.append(cardspec::kMaxSpecBytes - padded.size(), ' ');
    Check(padded.size() == cardspec::kMaxSpecBytes, "padded document is exactly 32768 bytes");
    ParseOk("document at 32768 bytes", padded);
    std::string bom_padded = std::string("\xEF\xBB\xBF") + GoodRowsDoc();
    bom_padded.append(cardspec::kMaxSpecBytes - bom_padded.size(), ' ');
    Check(bom_padded.size() == cardspec::kMaxSpecBytes, "BOM document is exactly 32768 bytes");
    ParseOk("BOM document at 32768 bytes", bom_padded);
}

// --- Structural / schema error matrix -----------------------------------

void TestRootErrors() {
    const std::string rows = Arr({RowsItem(Q("l"), Q("t"))});

    ParseReject("root array", "[]", L"对象");
    ParseReject("root string", Q("x"), L"对象");
    ParseReject("empty object", "{}", L"root.version");

    ParseReject("missing version",
                Obj({{"title", Q("t")}, {"source", Inline()}, {"layout", Q("rows")},
                     {"rows", rows}}),
                L"root.version");
    ParseReject("missing title",
                Obj({{"version", "1"}, {"source", Inline()}, {"layout", Q("rows")},
                     {"rows", rows}}),
                L"root.title");
    ParseReject("missing source",
                Obj({{"version", "1"}, {"title", Q("t")}, {"layout", Q("rows")},
                     {"rows", rows}}),
                L"root.source");
    ParseReject("missing layout",
                Obj({{"version", "1"}, {"title", Q("t")}, {"source", Inline()},
                     {"rows", rows}}),
                L"root.layout");
    ParseReject("missing rows",
                Obj({{"version", "1"}, {"title", Q("t")}, {"source", Inline()},
                     {"layout", Q("rows")}}),
                L"root.rows");

    ParseReject("unknown root field",
                Obj({{"version", "1"}, {"title", Q("t")}, {"source", Inline()},
                     {"layout", Q("rows")}, {"rows", rows}, {"SECRET9f3", "1"}}),
                L"root");
    ParseReject("duplicate root title",
                "{\"version\":1,\"title\":\"t\",\"title\":\"u\",\"source\":" + Inline() +
                    ",\"layout\":\"rows\",\"rows\":" + rows + "}",
                L"重复键");
    ParseReject("duplicate root unknown key", "{\"a\":1,\"a\":2}", L"重复键");

    ParseReject("version bool",
                RootDoc("true", Q("t"), Inline(), Q("rows"), rows), L"root.version");
    ParseReject("version string",
                RootDoc(Q("1"), Q("t"), Inline(), Q("rows"), rows), L"root.version");
    ParseReject("version null", RootDoc("null", Q("t"), Inline(), Q("rows"), rows),
                L"root.version");
    ParseReject("version two", RootDoc("2", Q("t"), Inline(), Q("rows"), rows),
                L"root.version");
    ParseReject("version one point five",
                RootDoc("1.5", Q("t"), Inline(), Q("rows"), rows), L"root.version");
    ParseReject("version infinite",
                RootDoc("1e999", Q("t"), Inline(), Q("rows"), rows), L"root.version");

    ParseReject("layout unknown",
                RootDoc("1", Q("t"), Inline(), Q("grid"), rows), L"root.layout");
    ParseReject("layout case sensitive",
                RootDoc("1", Q("t"), Inline(), Q("Rows"), rows), L"root.layout");
    ParseReject("layout bool", RootDoc("1", Q("t"), Inline(), "true", rows), L"root.layout");
    ParseReject("layout number", RootDoc("1", Q("t"), Inline(), "1", rows), L"root.layout");

    ParseReject("title number", RootDoc("1", "1", Inline(), Q("rows"), rows), L"root.title");
    ParseReject("title bool", RootDoc("1", "true", Inline(), Q("rows"), rows), L"root.title");
    ParseReject("title array", RootDoc("1", "[]", Inline(), Q("rows"), rows), L"root.title");
    ParseReject("title object", RootDoc("1", "{}", Inline(), Q("rows"), rows), L"root.title");
    ParseReject("title empty", RootDoc("1", Q(""), Inline(), Q("rows"), rows), L"root.title");
    ParseReject("title 65 units",
                RootDoc("1", Q(Repeat('a', 65)), Inline(), Q("rows"), rows), L"root.title");
    ParseReject("title 33 emoji (66 units)",
                RootDoc("1", Q(U8(Emoji(33))), Inline(), Q("rows"), rows),
                L"root.title");

    ParseReject("source missing type", RootDoc("1", Q("t"), "{}", Q("rows"), rows),
                L"root.source.type");
    ParseReject("source unknown field",
                RootDoc("1", Q("t"), "{\"type\":\"inline\",\"url\":\"SECRET9f3\"}",
                        Q("rows"), rows),
                L"root.source");
    ParseReject("source duplicate type",
                RootDoc("1", Q("t"), "{\"type\":\"inline\",\"type\":\"inline\"}", Q("rows"),
                        rows),
                L"重复键");
    ParseReject("source array", RootDoc("1", Q("t"), "[]", Q("rows"), rows), L"root.source");
    ParseReject("source string", RootDoc("1", Q("t"), Q("inline"), Q("rows"), rows),
                L"root.source");
    ParseReject("source file", RootDoc("1", Q("t"), "{\"type\":\"file\"}", Q("rows"), rows),
                L"root.source.type");
    ParseReject("source http", RootDoc("1", Q("t"), "{\"type\":\"http\"}", Q("rows"), rows),
                L"root.source.type");
    ParseReject("source exec", RootDoc("1", Q("t"), "{\"type\":\"exec\"}", Q("rows"), rows),
                L"root.source.type");
    ParseReject("source llm", RootDoc("1", Q("t"), "{\"type\":\"llm\"}", Q("rows"), rows),
                L"root.source.type");
    ParseReject("source type bool",
                RootDoc("1", Q("t"), "{\"type\":true}", Q("rows"), rows), L"root.source.type");

    ParseReject("rows number", RootDoc("1", Q("t"), Inline(), Q("rows"), "1"), L"root.rows");
    ParseReject("rows object", RootDoc("1", Q("t"), Inline(), Q("rows"), "{}"), L"root.rows");
    ParseReject("rows empty", RootDoc("1", Q("t"), Inline(), Q("rows"), "[]"), L"root.rows");
    ParseReject("rows nine", RootDoc("1", Q("t"), Inline(), Q("rows"),
                                    Arr({RowsItem(Q("a"), Q("b")), RowsItem(Q("a"), Q("b")),
                                         RowsItem(Q("a"), Q("b")), RowsItem(Q("a"), Q("b")),
                                         RowsItem(Q("a"), Q("b")), RowsItem(Q("a"), Q("b")),
                                         RowsItem(Q("a"), Q("b")), RowsItem(Q("a"), Q("b")),
                                         RowsItem(Q("a"), Q("b"))})),
                L"root.rows");
}

void TestRowsRowErrors() {
    const std::string rows_prefix = "{\"version\":1,\"title\":\"t\",\"source\":" + Inline() +
                                    ",\"layout\":\"rows\",\"rows\":[";
    const std::string rows_suffix = "]}";

    ParseReject("rows item not object",
                rows_prefix + "[]" + rows_suffix, L"root.rows[0]");
    ParseReject("rows item missing label", rows_prefix + "{\"text\":\"t\"}" + rows_suffix,
                L"root.rows[0].label");
    ParseReject("rows item missing text", rows_prefix + "{\"label\":\"l\"}" + rows_suffix,
                L"root.rows[0].text");
    ParseReject("rows item unknown field",
                rows_prefix + "{\"label\":\"l\",\"text\":\"t\",\"value\":1}" + rows_suffix,
                L"root.rows[0]");
    ParseReject("rows item unknown field secret",
                rows_prefix + "{\"label\":\"l\",\"text\":\"t\",\"SECRET9f3\":1}" + rows_suffix,
                L"root.rows[0]");
    ParseReject("rows item duplicate label",
                rows_prefix + "{\"label\":\"l\",\"label\":\"l\",\"text\":\"t\"}" + rows_suffix,
                L"重复键");
    ParseReject("rows label empty",
                rows_prefix + RowsItem(Q(""), Q("t")) + rows_suffix, L"root.rows[0].label");
    ParseReject("rows label 33 units",
                rows_prefix + RowsItem(Q(Repeat('l', 33)), Q("t")) + rows_suffix,
                L"root.rows[0].label");
    ParseReject("rows label number",
                rows_prefix + RowsItem("1", Q("t")) + rows_suffix, L"root.rows[0].label");
    ParseReject("rows text empty",
                rows_prefix + RowsItem(Q("l"), Q("")) + rows_suffix, L"root.rows[0].text");
    ParseReject("rows text 97 units",
                rows_prefix + RowsItem(Q("l"), Q(Repeat('t', 97))) + rows_suffix,
                L"root.rows[0].text");
    ParseReject("rows text bool",
                rows_prefix + RowsItem(Q("l"), "true") + rows_suffix, L"root.rows[0].text");
    // Second row path is reported correctly.
    ParseReject("rows second row bad",
                rows_prefix + RowsItem(Q("l"), Q("t")) + "," + RowsItem(Q("l"), Q("")) +
                    rows_suffix,
                L"root.rows[1].text");
}

void TestBarRowErrors() {
    const std::string bar_prefix = "{\"version\":1,\"title\":\"t\",\"source\":" + Inline() +
                                   ",\"layout\":\"bar\",\"rows\":[";
    const std::string bar_suffix = "]}";

    ParseReject("bar item missing label",
                bar_prefix + "{\"value\":1,\"max\":2}" + bar_suffix, L"root.rows[0].label");
    ParseReject("bar item missing value",
                bar_prefix + "{\"label\":\"l\",\"max\":2}" + bar_suffix, L"root.rows[0].value");
    ParseReject("bar item missing max",
                bar_prefix + "{\"label\":\"l\",\"value\":1}" + bar_suffix, L"root.rows[0].max");
    ParseReject("bar item unknown text",
                bar_prefix + "{\"label\":\"l\",\"value\":1,\"max\":2,\"text\":\"t\"}" +
                    bar_suffix,
                L"root.rows[0]");
    ParseReject("bar item label empty",
                bar_prefix + BarItem(Q(""), "1", "2") + bar_suffix, L"root.rows[0].label");
    ParseReject("bar value bool",
                bar_prefix + BarItem(Q("l"), "true", "2") + bar_suffix, L"root.rows[0].value");
    ParseReject("bar value string",
                bar_prefix + BarItem(Q("l"), Q("1"), "2") + bar_suffix,
                L"root.rows[0].value");
    ParseReject("bar value null",
                bar_prefix + BarItem(Q("l"), "null", "2") + bar_suffix,
                L"root.rows[0].value");
    ParseReject("bar max bool",
                bar_prefix + BarItem(Q("l"), "1", "true") + bar_suffix, L"root.rows[0].max");
    ParseReject("bar max zero", bar_prefix + BarItem(Q("l"), "0", "0") + bar_suffix,
                L"必须大于 0");
    ParseReject("bar max negative", bar_prefix + BarItem(Q("l"), "0", "-1") + bar_suffix,
                L"必须大于 0");
    ParseReject("bar value negative", bar_prefix + BarItem(Q("l"), "-1", "2") + bar_suffix,
                L"0..max");
    ParseReject("bar value above max", bar_prefix + BarItem(Q("l"), "6", "5") + bar_suffix,
                L"0..max");
    ParseReject("bar value infinite",
                bar_prefix + BarItem(Q("l"), "1e999", "5") + bar_suffix, L"有限");
    ParseReject("bar max infinite",
                bar_prefix + BarItem(Q("l"), "1", "1e999") + bar_suffix, L"有限");
}

// --- Text strictness: UTF-8, controls, surrogates -----------------------

void TestTextStrictness() {
    const std::string rows = Arr({RowsItem(Q("l"), Q("t"))});
    const auto title_doc = [&rows](const std::string& title_literal) {
        return RootDoc("1", title_literal, Inline(), Q("rows"), rows);
    };

    ParseReject("title control U+0001", title_doc(Q("\\u0001")), L"控制字符");
    ParseReject("title NUL escape", title_doc(Q("\\u0000")), L"控制字符");
    ParseReject("title DEL", title_doc(Q("\\u007f")), L"控制字符");
    ParseReject("title newline escape", title_doc(Q("\\n")), L"控制字符");
    ParseReject("title tab escape", title_doc(Q("\\t")), L"控制字符");
    ParseReject("title lone high surrogate", title_doc(Q("\\ud800")), L"代理");
    ParseReject("title lone low surrogate", title_doc(Q("\\udc00")), L"代理");
    ParseReject("title unpaired surrogate pair",
                title_doc(Q("\\ud800\\ud800")), L"代理");

    ParseReject("invalid UTF-8 lone continuation",
                title_doc(Q(std::string("\x80", 1))), L"UTF-8");
    ParseReject("invalid UTF-8 overlong",
                title_doc(Q(std::string("\xC0\xAF", 2))), L"UTF-8");
    ParseReject("invalid UTF-8 encoded surrogate",
                title_doc(Q(std::string("\xED\xA0\x80", 3))), L"UTF-8");
    ParseReject("invalid UTF-8 truncated 3-byte",
                title_doc(Q(std::string("\xE4\xB8", 2))), L"UTF-8");
    ParseReject("invalid UTF-8 truncated 4-byte",
                title_doc(Q(std::string("\xF0\x9F\x98", 3))), L"UTF-8");
    ParseReject("invalid UTF-8 lead byte",
                title_doc(Q(std::string("\xFF", 1))), L"UTF-8");
    ParseReject("invalid UTF-8 in key",
                "{\"version\":1,\"title\":\"t\",\"source\":" + Inline() +
                    ",\"layout\":\"rows\",\"rows\":" + rows + ","
                    "\"" + std::string("\x80", 1) + "\":1}",
                L"UTF-8");
}

// --- JSON syntax, size, depth, API shape --------------------------------

void TestSyntaxAndLimits() {
    ParseReject("empty document", "", L"JSON");
    ParseReject("BOM only", std::string("\xEF\xBB\xBF"), L"JSON");
    ParseReject("unterminated object", "{", L"JSON");
    ParseReject("trailing comma", "{\"version\":1,}", L"JSON");
    ParseReject("trailing data", GoodRowsDoc() + "x", L"JSON");
    ParseReject("bad literal", RootDoc("tru", Q("t"), Inline(), Q("rows"), "[]"), L"JSON");
    ParseReject("bad number", RootDoc("1.", Q("t"), Inline(), Q("rows"), "[]"), L"JSON");
    ParseReject("raw control byte in string",
                RootDoc("1", "\"a\nb\"", Inline(), Q("rows"),
                        Arr({RowsItem(Q("l"), Q("t"))})),
                L"控制字符");

    ParseReject("document over 32768 bytes",
                GoodRowsDoc() + Repeat(' ', cardspec::kMaxSpecBytes), L"32768");

    std::string deep;
    deep.append(200, '[');
    deep.append(200, ']');
    ParseReject("deeply nested JSON", deep, L"嵌套");
    std::string deep_broken(200, '[');
    ParseReject("deeply broken JSON", deep_broken, L"嵌套");

    // Null out / null error are safe; success clears a stale error.
    std::wstring error = L"stale error";
    Check(!cardspec::Parse(GoodRowsDoc(), nullptr, &error), "null out is rejected");
    Check(!error.empty(), "null out sets an error");

    cardspec::Spec spec;
    error = L"stale error";
    Check(cardspec::Parse(GoodRowsDoc(), &spec, &error), "parse after stale error succeeds");
    Check(error.empty(), "success clears a stale error");
}

// --- Typed Validate -----------------------------------------------------

void TestTypedValidate() {
    {
        std::wstring error = L"stale error";
        Check(cardspec::Validate(MakeRowsSpec(), &error), "typed rows spec is valid");
        Check(error.empty(), "Validate success clears error");
        error = L"stale error";
        Check(cardspec::Validate(MakeBarSpec(), &error), "typed bar spec is valid");
        Check(error.empty(), "Validate bar success clears error");
    }
    // Unused fields never leak into the other layout.
    {
        cardspec::Spec spec = MakeRowsSpec();
        spec.rows[0].value = std::numeric_limits<double>::quiet_NaN();
        spec.rows[0].max = std::numeric_limits<double>::infinity();
        Check(cardspec::Validate(spec), "rows layout ignores bar numbers");
        spec = MakeBarSpec();
        spec.rows[0].text = L"";
        Check(cardspec::Validate(spec), "bar layout ignores text");
    }

    {
        cardspec::Spec spec = MakeRowsSpec();
        spec.layout = static_cast<cardspec::Layout>(42);
        ExpectValidateFail("invalid layout enum", spec, L"布局枚举");
    }
    {
        cardspec::Spec spec = MakeRowsSpec();
        spec.title.clear();
        ExpectValidateFail("typed empty title", spec, L"root.title");
    }
    {
        cardspec::Spec spec = MakeRowsSpec();
        spec.title = std::wstring(65, L'a');
        ExpectValidateFail("typed title 65 units", spec, L"root.title");
    }
    {
        cardspec::Spec spec = MakeRowsSpec();
        spec.title = L"a\x01" L"b";
        ExpectValidateFail("typed title control", spec, L"控制字符");
    }
    {
        cardspec::Spec spec = MakeRowsSpec();
        spec.title = L"a\x7f" L"b";
        ExpectValidateFail("typed title DEL", spec, L"控制字符");
    }
    {
        cardspec::Spec spec = MakeRowsSpec();
        spec.title = L"\xD800";
        ExpectValidateFail("typed title lone surrogate", spec, L"代理");
    }
    {
        cardspec::Spec spec = MakeRowsSpec();
        spec.rows.clear();
        ExpectValidateFail("typed no rows", spec, L"root.rows");
    }
    {
        cardspec::Spec spec = MakeRowsSpec();
        spec.rows.assign(9, cardspec::Row{L"l", L"t", 0.0, 1.0});
        ExpectValidateFail("typed nine rows", spec, L"root.rows");
    }
    {
        cardspec::Spec spec = MakeRowsSpec();
        spec.rows[0].label.clear();
        ExpectValidateFail("typed empty label", spec, L"root.rows[0].label");
    }
    {
        cardspec::Spec spec = MakeRowsSpec();
        spec.rows[0].label = std::wstring(33, L'l');
        ExpectValidateFail("typed label 33 units", spec, L"root.rows[0].label");
    }
    {
        cardspec::Spec spec = MakeRowsSpec();
        spec.rows[0].text.clear();
        ExpectValidateFail("typed empty text", spec, L"root.rows[0].text");
    }
    {
        cardspec::Spec spec = MakeRowsSpec();
        spec.rows[0].text = std::wstring(97, L't');
        ExpectValidateFail("typed text 97 units", spec, L"root.rows[0].text");
    }
    {
        cardspec::Spec spec = MakeBarSpec();
        spec.rows[0].value = std::numeric_limits<double>::quiet_NaN();
        ExpectValidateFail("typed NaN value", spec, L"有限");
    }
    {
        cardspec::Spec spec = MakeBarSpec();
        spec.rows[0].max = std::numeric_limits<double>::infinity();
        ExpectValidateFail("typed infinite max", spec, L"有限");
    }
    {
        cardspec::Spec spec = MakeBarSpec();
        spec.rows[0].max = 0.0;
        ExpectValidateFail("typed zero max", spec, L"必须大于 0");
    }
    {
        cardspec::Spec spec = MakeBarSpec();
        spec.rows[0].value = -1.0;
        ExpectValidateFail("typed negative value", spec, L"0..max");
    }
    {
        cardspec::Spec spec = MakeBarSpec();
        spec.rows[0].value = 6.0;
        ExpectValidateFail("typed value above max", spec, L"0..max");
    }
}

// --- Deterministic serialization ---------------------------------------

void TestSerialize() {
    const cardspec::Spec rows = MakeRowsSpec();
    const std::string expected_rows =
        Obj({{"version", "1"},
             {"title", Q(U8(L"今日目标"))},
             {"source", Inline()},
             {"layout", Q("rows")},
             {"rows", Arr({Obj({{"label", Q(U8(L"学习"))},
                                {"text", Q(U8(L"阅读三十分钟"))}})})}});
    std::wstring error = L"stale error";
    const std::string rows_json = cardspec::Serialize(rows, &error);
    Check(rows_json == expected_rows, "rows serialization is exact and ordered");
    Check(error.empty(), "Serialize success clears error");
    Check(rows_json == cardspec::Serialize(rows), "rows serialization is deterministic");

    const cardspec::Spec bar = MakeBarSpec();
    const std::string expected_bar =
        Obj({{"version", "1"},
             {"title", Q(U8(L"目标进度"))},
             {"source", Inline()},
             {"layout", Q("bar")},
             {"rows", Arr({Obj({{"label", Q(U8(L"学习"))},
                                {"value", "3"},
                                {"max", "5"}})})}});
    const std::string bar_json = cardspec::Serialize(bar);
    Check(bar_json == expected_bar, "bar serialization is exact and ordered");
    Check(bar_json == cardspec::Serialize(bar), "bar serialization is deterministic");
    Check(bar_json.rfind("{\"version\":1,\"title\":", 0) == 0,
          "serialized output starts with version then title");

    // Round trips: valid serialization reparses to the same typed spec and
    // serializes back byte-identically.
    cardspec::Spec reparsed_rows;
    if (ParseOk("rows output reparses", rows_json, &reparsed_rows)) {
        Check(SameSpec(rows, reparsed_rows), "rows round trip preserves values");
        Check(cardspec::Serialize(reparsed_rows) == rows_json,
              "rows round trip is byte identical");
    }
    cardspec::Spec reparsed_bar;
    if (ParseOk("bar output reparses", bar_json, &reparsed_bar)) {
        Check(SameSpec(bar, reparsed_bar), "bar round trip preserves values");
        Check(cardspec::Serialize(reparsed_bar) == bar_json,
              "bar round trip is byte identical");
    }

    // Fractional values and escaped characters round trip too.
    cardspec::Spec fractional = MakeBarSpec();
    fractional.rows[0].value = 0.5;
    fractional.rows[0].max = 2.5;
    fractional.rows[0].label = L"引用\"\\\\";
    const std::string fractional_json = cardspec::Serialize(fractional);
    cardspec::Spec fractional_back;
    if (ParseOk("fractional output reparses", fractional_json, &fractional_back)) {
        Check(SameSpec(fractional, fractional_back), "fractional/escaped round trip");
    }

    cardspec::Spec emoji = MakeRowsSpec();
    emoji.title = Emoji(1);
    emoji.rows[0].text = Emoji(2);
    cardspec::Spec emoji_back;
    if (ParseOk("emoji output reparses", cardspec::Serialize(emoji), &emoji_back)) {
        Check(SameSpec(emoji, emoji_back), "emoji round trip");
    }

    // Invalid typed input: empty output, error names the field.
    cardspec::Spec invalid = MakeRowsSpec();
    invalid.title.clear();
    error = L"stale error";
    Check(cardspec::Serialize(invalid, &error).empty(), "invalid typed input returns empty");
    Check(!error.empty() && error.find(L"root.title") != std::wstring::npos,
          "invalid typed input reports the field path");

    cardspec::Spec null_bytes = MakeRowsSpec();
    null_bytes.title = L"\xD800";
    std::wstring surrogate_error;
    Check(cardspec::Serialize(null_bytes, &surrogate_error).empty(),
          "lone surrogate serializes to empty");
    Check(surrogate_error.find(L"代理") != std::wstring::npos,
          "lone surrogate error names the surrogate problem");

    // Default Spec is invalid but never crashes with a null error pointer.
    Check(!cardspec::Validate(cardspec::Spec{}), "default spec is invalid");
    Check(cardspec::Serialize(cardspec::Spec{}).empty(), "default spec serializes to empty");
}

} // namespace

int wmain() {
    TestLegalDocuments();
    TestRootErrors();
    TestRowsRowErrors();
    TestBarRowErrors();
    TestTextStrictness();
    TestSyntaxAndLimits();
    TestTypedValidate();
    TestSerialize();

    std::printf("[cardspec-test] %s\n", failures == 0 ? "ok" : "failed");
    return failures == 0 ? 0 : 1;
}
