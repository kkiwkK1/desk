// M2.1 source-core tests: strict generic JSON facade, closed v2 HTTP schema,
// RFC 6901 pointers and pure response binding. The test builds every document
// and response in memory; it never touches files, network, credentials, UI or
// user data, and it does not depend on the network/UI lanes.
#include "data/cardspec/http_source.h"

#include "data/cardspec/cardspec.h"
#include "data/cardspec/strict_json.h"
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
        std::printf("[cardspec-source-test] FAIL %s\n", what.c_str());
        ++failures;
    }
}

std::string U8(std::wstring_view text) { return assistant::ToUtf8(text); }

std::string Repeat(char c, std::size_t count) { return std::string(count, c); }

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

std::string RootDoc(const std::string& version, const std::string& title,
                    const std::string& source, const std::string& layout,
                    const std::string& rows) {
    return Obj({{"version", version},
                {"title", title},
                {"source", source},
                {"layout", layout},
                {"rows", rows}});
}

std::string Inline() { return "{\"type\":\"inline\"}"; }

std::string HttpSource(std::string profile = "demo") {
    return Obj({{"type", Q("http")}, {"profile", Q(profile)}});
}

std::string V2RowsDoc(const std::string& rows, const std::string& title = "",
                      const std::string& source = "") {
    return RootDoc("2", title.empty() ? Q(U8(L"标题")) : title,
                   source.empty() ? HttpSource() : source, Q("rows"), rows);
}

std::string V2BarDoc(const std::string& rows) {
    return RootDoc("2", Q(U8(L"标题")), HttpSource(), Q("bar"), rows);
}

std::string V1RowsItem(const std::string& label, const std::string& text) {
    return Obj({{"label", label}, {"text", text}});
}

std::string V1BarItem(const std::string& label, const std::string& value,
                      const std::string& max) {
    return Obj({{"label", label}, {"value", value}, {"max", max}});
}

std::string HttpRowsItem(const std::string& label, const std::string& text_pointer) {
    return Obj({{"label", label}, {"text_path", text_pointer}});
}

std::string HttpBarConstItem(const std::string& label, const std::string& value_pointer,
                             const std::string& max) {
    return Obj({{"label", label}, {"value_path", value_pointer}, {"max", max}});
}

std::string HttpBarPointerItem(const std::string& label, const std::string& value_pointer,
                               const std::string& max_pointer) {
    return Obj({{"label", label},
                {"value_path", value_pointer},
                {"max_path", max_pointer}});
}

std::string GoodV1Rows() {
    return RootDoc("1", Q(U8(L"今日目标")), Inline(), Q("rows"),
                   Arr({V1RowsItem(Q(U8(L"学习")), Q(U8(L"阅读三十分钟")))}));
}

std::string GoodV1Bar() {
    return RootDoc("1", Q(U8(L"目标进度")), Inline(), Q("bar"),
                   Arr({V1BarItem(Q(U8(L"学习")), "3", "5")}));
}

std::string GoodV2Rows() {
    return V2RowsDoc(Arr({HttpRowsItem(Q(U8(L"名称")), Q("/full_name"))}));
}

std::string GoodV2Bar() {
    return V2BarDoc(Arr({HttpBarConstItem(Q(U8(L"进度")), Q("/done"), "5")}));
}

bool ParseSourceOk(const std::string& label, const std::string& text,
                   cardspec::SourceDocument* out = nullptr) {
    cardspec::SourceDocument local;
    cardspec::SourceDocument* target = out ? out : &local;
    std::wstring error = L"stale error";
    const bool ok = cardspec::ParseSourceDocument(text, target, &error);
    Check(ok, label + ": parses");
    if (ok) Check(error.empty(), label + ": success clears error");
    return ok;
}

void ParseSourceReject(const std::string& label, const std::string& text,
                       const std::wstring& fragment = L"") {
    cardspec::SourceDocument sentinel;
    sentinel.kind = cardspec::SourceKind::Http;
    sentinel.httpSpec.title = L"OUT_SENTINEL";
    sentinel.httpSpec.layout = cardspec::Layout::Bar;
    sentinel.httpSpec.profile = "keep";
    cardspec::HttpRow row;
    row.label = L"keep";
    row.valuePointer = "/v";
    row.max = 2.0;
    sentinel.httpSpec.rows.push_back(row);

    std::wstring error = L"stale error";
    const bool ok = cardspec::ParseSourceDocument(text, &sentinel, &error);
    Check(!ok, label + ": rejected");
    if (ok) return;
    Check(!error.empty(), label + ": error message is set");
    Check(error != L"stale error", label + ": error replaces stale error");
    Check(error.find(L"OUT_SENTINEL") == std::wstring::npos,
          label + ": error does not echo payload");
    Check(error.find(L"SECRET9f3") == std::wstring::npos,
          label + ": error does not echo secret sentinel");
    Check(sentinel.kind == cardspec::SourceKind::Http &&
              sentinel.httpSpec.title == L"OUT_SENTINEL" &&
              sentinel.httpSpec.layout == cardspec::Layout::Bar &&
              sentinel.httpSpec.profile == "keep" &&
              sentinel.httpSpec.rows.size() == 1 &&
              sentinel.httpSpec.rows[0].label == L"keep" &&
              sentinel.httpSpec.rows[0].valuePointer == "/v" &&
              sentinel.httpSpec.rows[0].max == 2.0,
          label + ": failure leaves out untouched");
    if (!fragment.empty()) {
        Check(error.find(fragment) != std::wstring::npos,
              label + ": error mentions " + assistant::ToUtf8(fragment));
    }
}

cardspec::HttpSpec RowsSpec(const std::string& pointer) {
    cardspec::HttpSpec spec;
    spec.title = L"标题";
    spec.layout = cardspec::Layout::Rows;
    spec.profile = "demo";
    cardspec::HttpRow row;
    row.label = L"值";
    row.textPointer = pointer;
    spec.rows.push_back(row);
    return spec;
}

cardspec::HttpSpec BarConstSpec(const std::string& value_pointer, double max) {
    cardspec::HttpSpec spec;
    spec.title = L"进度";
    spec.layout = cardspec::Layout::Bar;
    spec.profile = "demo";
    cardspec::HttpRow row;
    row.label = L"值";
    row.valuePointer = value_pointer;
    row.maxFromPointer = false;
    row.max = max;
    spec.rows.push_back(row);
    return spec;
}

cardspec::HttpSpec BarPointerSpec(const std::string& value_pointer,
                                  const std::string& max_pointer) {
    cardspec::HttpSpec spec;
    spec.title = L"进度";
    spec.layout = cardspec::Layout::Bar;
    spec.profile = "demo";
    cardspec::HttpRow row;
    row.label = L"值";
    row.valuePointer = value_pointer;
    row.maxFromPointer = true;
    row.maxPointer = max_pointer;
    spec.rows.push_back(row);
    return spec;
}

bool DecodeOk(const std::string& label, const cardspec::HttpSpec& spec,
              const std::string& body, cardspec::Spec* out = nullptr) {
    cardspec::Spec local;
    cardspec::Spec* target = out ? out : &local;
    std::wstring error = L"stale error";
    const bool ok = cardspec::DecodeHttpResponse(spec, body, target, &error);
    Check(ok, label + ": decodes");
    if (ok) Check(error.empty(), label + ": success clears error");
    return ok;
}

void DecodeReject(const std::string& label, const cardspec::HttpSpec& spec,
                  const std::string& body, const std::wstring& fragment = L"",
                  const std::wstring& forbidden = L"") {
    cardspec::Spec sentinel;
    sentinel.title = L"OUT_SENTINEL";
    sentinel.layout = cardspec::Layout::Bar;
    cardspec::Row row;
    row.label = L"keep";
    row.value = 1.0;
    row.max = 2.0;
    sentinel.rows.push_back(row);

    std::wstring error = L"stale error";
    const bool ok = cardspec::DecodeHttpResponse(spec, body, &sentinel, &error);
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
    if (!forbidden.empty()) {
        Check(error.find(forbidden) == std::wstring::npos,
              label + ": error does not echo forbidden token");
    }
    if (!fragment.empty()) {
        Check(error.find(fragment) != std::wstring::npos,
              label + ": error mentions " + assistant::ToUtf8(fragment));
    }
}

bool StrictOk(const std::string& label, const std::string& text,
              assistant::JsonValue* out = nullptr) {
    assistant::JsonValue local;
    assistant::JsonValue* target = out ? out : &local;
    std::wstring error = L"stale error";
    const bool ok = cardspec::ParseStrictJson(text, target, &error);
    Check(ok, label + ": parses");
    if (ok) Check(error.empty(), label + ": success clears error");
    return ok;
}

void StrictReject(const std::string& label, const std::string& text,
                  const std::wstring& fragment = L"") {
    assistant::JsonValue sentinel = assistant::JsonValue::MakeString("OUT_SENTINEL");
    std::wstring error = L"stale error";
    const bool ok = cardspec::ParseStrictJson(text, &sentinel, &error);
    Check(!ok, label + ": rejected");
    if (ok) return;
    Check(!error.empty(), label + ": error message is set");
    Check(error != L"stale error", label + ": error replaces stale error");
    Check(error.find(L"OUT_SENTINEL") == std::wstring::npos,
          label + ": error does not echo payload");
    Check(error.find(L"SECRET9f3") == std::wstring::npos,
          label + ": error does not echo secret sentinel");
    Check(sentinel.IsString() && sentinel.String() == "OUT_SENTINEL",
          label + ": failure leaves out untouched");
    if (!fragment.empty()) {
        Check(error.find(fragment) != std::wstring::npos,
              label + ": error mentions " + assistant::ToUtf8(fragment));
    }
}

// --- v1 compatibility ---------------------------------------------------

void TestV1Compatibility() {
    cardspec::SourceDocument rows;
    if (ParseSourceOk("v1 rows via source document", GoodV1Rows(), &rows)) {
        Check(rows.kind == cardspec::SourceKind::Inline, "v1 rows kind is inline");
        Check(rows.inlineSpec.title == L"今日目标", "v1 rows title");
        Check(rows.inlineSpec.layout == cardspec::Layout::Rows, "v1 rows layout");
        Check(rows.inlineSpec.rows.size() == 1, "v1 rows count");
        Check(rows.inlineSpec.rows[0].label == L"学习", "v1 rows label");
        Check(rows.inlineSpec.rows[0].text == L"阅读三十分钟", "v1 rows text");
        Check(cardspec::Serialize(rows.inlineSpec) == GoodV1Rows(),
              "v1 rows serialization is unchanged");
    }

    cardspec::SourceDocument bar;
    if (ParseSourceOk("v1 bar via source document", GoodV1Bar(), &bar)) {
        Check(bar.kind == cardspec::SourceKind::Inline, "v1 bar kind is inline");
        Check(bar.inlineSpec.layout == cardspec::Layout::Bar, "v1 bar layout");
        Check(bar.inlineSpec.rows.size() == 1 && bar.inlineSpec.rows[0].value == 3.0 &&
                  bar.inlineSpec.rows[0].max == 5.0,
              "v1 bar values");
        Check(cardspec::Serialize(bar.inlineSpec) == GoodV1Bar(),
              "v1 bar serialization is unchanged");
    }

    ParseSourceOk("v1 leading BOM", std::string("\xEF\xBB\xBF") + GoodV1Rows());
    ParseSourceOk("v1 version 1.0", RootDoc("1.0", Q("t"), Inline(), Q("rows"),
                                            Arr({V1RowsItem(Q("l"), Q("t"))})));

    ParseSourceReject("v1 http is still inline-only",
                      RootDoc("1", Q("t"), "{\"type\":\"http\"}", Q("rows"),
                              Arr({HttpRowsItem(Q("l"), Q("/x"))})),
                      L"root.source.type");
    ParseSourceReject("v1 unknown root field",
                      Obj({{"version", "1"},
                           {"title", Q("t")},
                           {"source", Inline()},
                           {"layout", Q("rows")},
                           {"rows", Arr({V1RowsItem(Q("l"), Q("t"))})},
                           {"SECRET9f3", "1"}}),
                      L"root");
    ParseSourceReject("v1 version two is not accepted by v1 parser",
                      RootDoc("2", Q("t"), Inline(), Q("rows"),
                              Arr({V1RowsItem(Q("l"), Q("t"))})),
                      L"root.source.type");
    ParseSourceReject("v1 duplicate title",
                      "{\"version\":1,\"title\":\"t\",\"title\":\"u\",\"source\":" +
                          Inline() + ",\"layout\":\"rows\",\"rows\":" +
                          Arr({V1RowsItem(Q("l"), Q("t"))}) + "}",
                      L"重复键");
}

// --- strict generic JSON facade ----------------------------------------

void TestStrictJsonFacade() {
    assistant::JsonValue value;
    if (StrictOk("strict root number", "42", &value)) {
        Check(value.IsNumber() && value.Number() == 42.0, "number root decoded");
    }
    if (StrictOk("strict root string", Q("text"), &value)) {
        Check(value.IsString() && value.String() == "text", "string root decoded");
    }
    if (StrictOk("strict root null", "null", &value)) {
        Check(value.IsNull(), "null root decoded");
    }
    if (StrictOk("strict root bool", "true", &value)) {
        Check(value.IsBool() && value.Bool(), "bool root decoded");
    }
    if (StrictOk("strict root array", "[]", &value)) {
        Check(value.IsArray() && value.Items().empty(), "array root decoded");
    }
    if (StrictOk("strict root object", Obj({{"a", "1"}}), &value)) {
        Check(value.IsObject() && value.Has("a"), "object root decoded");
    }
    StrictOk("strict leading BOM", std::string("\xEF\xBB\xBF") + Obj({{"a", "1"}}));
    StrictOk("strict arbitrary whitespace", " \r\n\t42 \n");

    // Escaped controls are legal JSON and must not be rejected globally.
    assistant::JsonValue controls;
    if (StrictOk("strict escaped controls",
                 Obj({{"unused", Q("line1\\nline2")}, {"ctrl", Q("\\u0001")}}),
                 &controls)) {
        Check(controls.Has("unused") && controls.Has("ctrl"),
              "escaped controls remain available to the schema layer");
    }

    StrictReject("strict duplicate key", "{\"a\":1,\"a\":2}", L"重复键");
    StrictReject("strict escaped duplicate key", "{\"a\":1,\"\\u0061\":2}", L"重复键");
    StrictReject("strict duplicate nested key", "{\"a\":{\"b\":1,\"b\":2}}", L"重复键");
    StrictReject("strict trailing data", "42x", L"JSON");
    StrictReject("strict empty document", "", L"JSON");
    StrictReject("strict BOM only", std::string("\xEF\xBB\xBF"), L"JSON");
    StrictReject("strict invalid UTF-8", std::string("{\"a\":\"\x80\"}"), L"UTF-8");
    StrictReject("strict invalid UTF-8 key", std::string("{\"\x80\":1}"), L"UTF-8");

    std::string deep;
    deep.append(200, '[');
    deep.append(200, ']');
    StrictReject("strict depth limit", deep, L"嵌套");

    const std::string oversized =
        "{\"x\":\"" + Repeat('a', cardspec::kMaxSpecBytes) + "\"}";
    StrictReject("strict size limit", oversized, L"32768");

    std::string sized = Obj({{"a", "1"}});
    sized.append(cardspec::kMaxSpecBytes - sized.size(), ' ');
    Check(sized.size() == cardspec::kMaxSpecBytes, "strict document is exactly 32768 bytes");
    StrictOk("strict document at 32768 bytes", sized);
    StrictReject("strict document over 32768 bytes", sized + " ", L"32768");
}

// --- closed v2 schema: legal shapes ------------------------------------

void TestV2Legal() {
    cardspec::SourceDocument rows;
    if (ParseSourceOk("v2 rows", GoodV2Rows(), &rows)) {
        Check(rows.kind == cardspec::SourceKind::Http, "v2 rows kind is http");
        Check(rows.httpSpec.title == L"标题", "v2 rows title");
        Check(rows.httpSpec.layout == cardspec::Layout::Rows, "v2 rows layout");
        Check(rows.httpSpec.profile == "demo", "v2 rows profile");
        Check(rows.httpSpec.rows.size() == 1, "v2 rows count");
        Check(rows.httpSpec.rows[0].label == L"名称", "v2 rows label");
        Check(rows.httpSpec.rows[0].textPointer == "/full_name", "v2 rows pointer");
    }

    cardspec::SourceDocument bar;
    if (ParseSourceOk("v2 bar constant max", GoodV2Bar(), &bar)) {
        Check(bar.kind == cardspec::SourceKind::Http, "v2 bar kind is http");
        Check(bar.httpSpec.layout == cardspec::Layout::Bar, "v2 bar layout");
        Check(bar.httpSpec.rows.size() == 1, "v2 bar count");
        Check(bar.httpSpec.rows[0].valuePointer == "/done", "v2 bar value pointer");
        Check(!bar.httpSpec.rows[0].maxFromPointer, "v2 bar constant max tagged");
        Check(bar.httpSpec.rows[0].max == 5.0, "v2 bar constant max value");
    }

    cardspec::SourceDocument bar_pointer;
    if (ParseSourceOk("v2 bar pointer max",
                      V2BarDoc(Arr({HttpBarPointerItem(Q("值"), Q("/done"), Q("/total"))})),
                      &bar_pointer)) {
        Check(bar_pointer.httpSpec.rows[0].maxFromPointer, "v2 bar pointer max tagged");
        Check(bar_pointer.httpSpec.rows[0].maxPointer == "/total",
              "v2 bar pointer max value");
    }

    ParseSourceOk("v2 leading BOM", std::string("\xEF\xBB\xBF") + GoodV2Rows());
    ParseSourceOk("v2 numeric 2.0", RootDoc("2.0", Q("t"), HttpSource(), Q("rows"),
                                            Arr({HttpRowsItem(Q("l"), Q(""))})));
    ParseSourceOk("v2 profile max 32",
                  RootDoc("2", Q("t"), HttpSource(Repeat('a', 32)), Q("rows"),
                          Arr({HttpRowsItem(Q("l"), Q(""))})));
    ParseSourceOk("v2 profile character set",
                  RootDoc("2", Q("t"), HttpSource("A-z_0-9"), Q("rows"),
                          Arr({HttpRowsItem(Q("l"), Q(""))})));
    ParseSourceOk("v2 empty root pointer",
                  RootDoc("2", Q("t"), HttpSource(), Q("rows"),
                          Arr({HttpRowsItem(Q("l"), Q(""))})));
    ParseSourceOk("v2 eight rows",
                  V2RowsDoc(Arr({HttpRowsItem(Q("a"), Q("")), HttpRowsItem(Q("a"), Q("")),
                                 HttpRowsItem(Q("a"), Q("")), HttpRowsItem(Q("a"), Q("")),
                                 HttpRowsItem(Q("a"), Q("")), HttpRowsItem(Q("a"), Q("")),
                                 HttpRowsItem(Q("a"), Q("")), HttpRowsItem(Q("a"), Q(""))})));

    std::string sized = GoodV2Rows();
    sized.append(cardspec::kMaxSpecBytes - sized.size(), ' ');
    Check(sized.size() == cardspec::kMaxSpecBytes, "v2 document is exactly 32768 bytes");
    ParseSourceOk("v2 document at 32768 bytes", sized);
}

// --- closed v2 schema: root/source errors ------------------------------

void TestV2RootAndSourceErrors() {
    const std::string rows = Arr({HttpRowsItem(Q("l"), Q(""))});

    ParseSourceReject("v2 root array", "[]", L"对象");
    ParseSourceReject("v2 root string", Q("x"), L"对象");
    ParseSourceReject("v2 missing version",
                      Obj({{"title", Q("t")}, {"source", HttpSource()}, {"layout", Q("rows")},
                           {"rows", rows}}),
                      L"root.version");
    ParseSourceReject("v2 missing title",
                      Obj({{"version", "2"}, {"source", HttpSource()}, {"layout", Q("rows")},
                           {"rows", rows}}),
                      L"root.title");
    ParseSourceReject("v2 missing source",
                      Obj({{"version", "2"}, {"title", Q("t")}, {"layout", Q("rows")},
                           {"rows", rows}}),
                      L"root.source");
    ParseSourceReject("v2 missing layout",
                      Obj({{"version", "2"}, {"title", Q("t")}, {"source", HttpSource()},
                           {"rows", rows}}),
                      L"root.layout");
    ParseSourceReject("v2 missing rows",
                      Obj({{"version", "2"}, {"title", Q("t")}, {"source", HttpSource()},
                           {"layout", Q("rows")}}),
                      L"root.rows");

    ParseSourceReject("v2 unknown root field",
                      Obj({{"version", "2"}, {"title", Q("t")}, {"source", HttpSource()},
                           {"layout", Q("rows")}, {"rows", rows},
                           {"SECRET9f3", Q("SECRET9f3")}}),
                      L"root");
    ParseSourceReject("v2 authority root url",
                      Obj({{"version", "2"}, {"title", Q("t")}, {"source", HttpSource()},
                           {"layout", Q("rows")}, {"rows", rows},
                           {"url", Q("SECRET9f3")}}),
                      L"root");

    ParseSourceReject("v2 version bool", RootDoc("true", Q("t"), HttpSource(), Q("rows"), rows),
                      L"root.version");
    ParseSourceReject("v2 version string", RootDoc(Q("2"), Q("t"), HttpSource(), Q("rows"), rows),
                      L"root.version");
    ParseSourceReject("v2 version 1.5", RootDoc("1.5", Q("t"), HttpSource(), Q("rows"), rows),
                      L"root.version");
    ParseSourceReject("v2 version infinite",
                      RootDoc("2e999", Q("t"), HttpSource(), Q("rows"), rows),
                      L"root.version");

    ParseSourceReject("v2 source missing type",
                      RootDoc("2", Q("t"), Obj({{"profile", Q("demo")}}), Q("rows"), rows),
                      L"root.source.type");
    ParseSourceReject("v2 source missing profile",
                      RootDoc("2", Q("t"), Obj({{"type", Q("http")}}), Q("rows"), rows),
                      L"root.source.profile");
    ParseSourceReject("v2 source file",
                      RootDoc("2", Q("t"), "{\"type\":\"file\",\"profile\":\"demo\"}",
                              Q("rows"), rows),
                      L"root.source.type");
    ParseSourceReject("v2 source inline",
                      RootDoc("2", Q("t"), Inline(), Q("rows"), rows),
                      L"root.source.type");
    ParseSourceReject("v2 source type bool",
                      RootDoc("2", Q("t"), "{\"type\":true,\"profile\":\"demo\"}",
                              Q("rows"), rows),
                      L"root.source.type");
    ParseSourceReject("v2 source array", RootDoc("2", Q("t"), "[]", Q("rows"), rows),
                      L"root.source");

    ParseSourceReject("v2 source url injection",
                      RootDoc("2", Q("t"),
                              "{\"type\":\"http\",\"profile\":\"demo\","
                              "\"url\":\"SECRET9f3\"}",
                              Q("rows"), rows),
                      L"root.source");
    ParseSourceReject("v2 source headers injection",
                      RootDoc("2", Q("t"),
                              "{\"type\":\"http\",\"profile\":\"demo\","
                              "\"headers\":{\"Secret9f3\":\"SECRET9f3\"}}",
                              Q("rows"), rows),
                      L"root.source");
    ParseSourceReject("v2 source allow injection",
                      RootDoc("2", Q("t"),
                              "{\"type\":\"http\",\"profile\":\"demo\","
                              "\"allow\":[\"SECRET9f3\"]}",
                              Q("rows"), rows),
                      L"root.source");

    ParseSourceReject("v2 profile empty",
                      RootDoc("2", Q("t"), HttpSource(""), Q("rows"), rows),
                      L"root.source.profile");
    ParseSourceReject("v2 profile 33 chars",
                      RootDoc("2", Q("t"), HttpSource(Repeat('a', 33)), Q("rows"), rows),
                      L"root.source.profile");
    ParseSourceReject("v2 profile space",
                      RootDoc("2", Q("t"), HttpSource("a b"), Q("rows"), rows),
                      L"root.source.profile");
    ParseSourceReject("v2 profile slash",
                      RootDoc("2", Q("t"), HttpSource("a/b"), Q("rows"), rows),
                      L"root.source.profile");
    ParseSourceReject("v2 profile unicode",
                      RootDoc("2", Q("t"), HttpSource(U8(L"演示")), Q("rows"), rows),
                      L"root.source.profile");
    ParseSourceReject("v2 profile non-string",
                      RootDoc("2", Q("t"), "{\"type\":\"http\",\"profile\":42}",
                              Q("rows"), rows),
                      L"root.source.profile");

    ParseSourceReject("v2 layout unknown",
                      RootDoc("2", Q("t"), HttpSource(), Q("grid"), rows), L"root.layout");
    ParseSourceReject("v2 layout case sensitive",
                      RootDoc("2", Q("t"), HttpSource(), Q("Rows"), rows), L"root.layout");
    ParseSourceReject("v2 layout bool",
                      RootDoc("2", Q("t"), HttpSource(), "true", rows), L"root.layout");

    ParseSourceReject("v2 title number",
                      RootDoc("2", "1", HttpSource(), Q("rows"), rows), L"root.title");
    ParseSourceReject("v2 title empty",
                      RootDoc("2", Q(""), HttpSource(), Q("rows"), rows), L"root.title");
    ParseSourceReject("v2 title control",
                      RootDoc("2", Q("\\u0001"), HttpSource(), Q("rows"), rows),
                      L"控制字符");
    ParseSourceReject("v2 title 65 units",
                      RootDoc("2", Q(Repeat('a', 65)), HttpSource(), Q("rows"), rows),
                      L"root.title");

    ParseSourceReject("v2 rows object",
                      RootDoc("2", Q("t"), HttpSource(), Q("rows"), "{}"), L"root.rows");
    ParseSourceReject("v2 rows empty",
                      RootDoc("2", Q("t"), HttpSource(), Q("rows"), "[]"), L"root.rows");
    ParseSourceReject("v2 rows nine",
                      V2RowsDoc(Arr({HttpRowsItem(Q("a"), Q("")), HttpRowsItem(Q("a"), Q("")),
                                     HttpRowsItem(Q("a"), Q("")), HttpRowsItem(Q("a"), Q("")),
                                     HttpRowsItem(Q("a"), Q("")), HttpRowsItem(Q("a"), Q("")),
                                     HttpRowsItem(Q("a"), Q("")), HttpRowsItem(Q("a"), Q("")),
                                     HttpRowsItem(Q("a"), Q(""))})),
                      L"root.rows");
}

// --- closed v2 schema: row/bar errors ----------------------------------

void TestV2RowsAndBarErrors() {
    ParseSourceReject("v2 rows item not object",
                      V2RowsDoc(Arr({"[]"})), L"root.rows[0]");
    ParseSourceReject("v2 row missing label",
                      V2RowsDoc(Arr({Obj({{"text_path", Q("/x")}})})), L"root.rows[0].label");
    ParseSourceReject("v2 row missing text_path",
                      V2RowsDoc(Arr({Obj({{"label", Q("l")}})})),
                      L"root.rows[0].text_path");
    ParseSourceReject("v2 row label number",
                      V2RowsDoc(Arr({HttpRowsItem("1", Q("/x"))})), L"root.rows[0].label");
    ParseSourceReject("v2 row label empty",
                      V2RowsDoc(Arr({HttpRowsItem(Q(""), Q("/x"))})), L"root.rows[0].label");
    ParseSourceReject("v2 row label 33 units",
                      V2RowsDoc(Arr({HttpRowsItem(Q(Repeat('l', 33)), Q("/x"))})),
                      L"root.rows[0].label");
    ParseSourceReject("v2 row label control",
                      V2RowsDoc(Arr({HttpRowsItem(Q("\\u0001"), Q("/x"))})), L"控制字符");
    ParseSourceReject("v2 row text_path number",
                      V2RowsDoc(Arr({Obj({{"label", Q("l")}, {"text_path", "1"}})})),
                      L"root.rows[0].text_path");
    ParseSourceReject("v2 row unknown field",
                      V2RowsDoc(Arr({Obj({{"label", Q("l")}, {"text_path", Q("/x")},
                                          {"text", Q("SECRET9f3")}})})),
                      L"root.rows[0]");
    ParseSourceReject("v2 row url injection",
                      V2RowsDoc(Arr({Obj({{"label", Q("l")}, {"text_path", Q("/x")},
                                          {"url", Q("SECRET9f3")}})})),
                      L"root.rows[0]");
    ParseSourceReject("v2 row second path is reported",
                      V2RowsDoc(Arr({HttpRowsItem(Q("l"), Q("")),
                                     HttpRowsItem(Q(""), Q(""))})),
                      L"root.rows[1].label");

    ParseSourceReject("v2 bar item unknown text_path",
                      V2BarDoc(Arr({Obj({{"label", Q("l")}, {"value_path", Q("/v")},
                                         {"text_path", Q("/x")}, {"max", "2"}})})),
                      L"root.rows[0]");
    ParseSourceReject("v2 bar missing label",
                      V2BarDoc(Arr({Obj({{"value_path", Q("/v")}, {"max", "2"}})})),
                      L"root.rows[0].label");
    ParseSourceReject("v2 bar missing value_path",
                      V2BarDoc(Arr({Obj({{"label", Q("l")}, {"max", "2"}})})),
                      L"root.rows[0].value_path");
    ParseSourceReject("v2 bar value_path non-string",
                      V2BarDoc(Arr({Obj({{"label", Q("l")}, {"value_path", "1"},
                                         {"max", "2"}})})),
                      L"root.rows[0].value_path");
    ParseSourceReject("v2 bar both max forms",
                      V2BarDoc(Arr({Obj({{"label", Q("l")}, {"value_path", Q("/v")},
                                         {"max", "2"}, {"max_path", Q("/m")}})})),
                      L"max");
    ParseSourceReject("v2 bar neither max form",
                      V2BarDoc(Arr({Obj({{"label", Q("l")}, {"value_path", Q("/v")}})})),
                      L"max");
    ParseSourceReject("v2 bar max string",
                      V2BarDoc(Arr({HttpBarConstItem(Q("l"), Q("/v"), Q("2"))})),
                      L"root.rows[0].max");
    ParseSourceReject("v2 bar max null",
                      V2BarDoc(Arr({HttpBarConstItem(Q("l"), Q("/v"), "null")})),
                      L"root.rows[0].max");
    ParseSourceReject("v2 bar max zero",
                      V2BarDoc(Arr({HttpBarConstItem(Q("l"), Q("/v"), "0")})),
                      L"必须大于 0");
    ParseSourceReject("v2 bar max negative",
                      V2BarDoc(Arr({HttpBarConstItem(Q("l"), Q("/v"), "-1")})),
                      L"必须大于 0");
    ParseSourceReject("v2 bar max infinite",
                      V2BarDoc(Arr({HttpBarConstItem(Q("l"), Q("/v"), "1e999")})),
                      L"有限");
    ParseSourceReject("v2 bar max_path number",
                      V2BarDoc(Arr({Obj({{"label", Q("l")}, {"value_path", Q("/v")},
                                         {"max_path", "1"}})})),
                      L"root.rows[0].max_path");
}

// --- duplicate keys at every layer -------------------------------------

void TestDuplicateKeys() {
    const std::string source = HttpSource();
    const std::string rows = Arr({HttpRowsItem(Q("l"), Q("/x"))});

    ParseSourceReject("duplicate root title",
                      "{\"version\":2,\"title\":\"t\",\"title\":\"u\",\"source\":" +
                          source + ",\"layout\":\"rows\",\"rows\":" + rows + "}",
                      L"重复键");
    ParseSourceReject("duplicate root escaped title",
                      "{\"version\":2,\"title\":\"t\",\"titl\\u0065\":\"u\",\"source\":" +
                          source + ",\"layout\":\"rows\",\"rows\":" + rows + "}",
                      L"重复键");
    ParseSourceReject("duplicate source type",
                      RootDoc("2", Q("t"),
                              "{\"type\":\"http\",\"type\":\"http\",\"profile\":\"demo\"}",
                              Q("rows"), rows),
                      L"重复键");
    ParseSourceReject("duplicate source escaped profile",
                      RootDoc("2", Q("t"),
                              "{\"type\":\"http\",\"profile\":\"demo\","
                              "\"profil\\u0065\":\"demo\"}",
                              Q("rows"), rows),
                      L"重复键");
    ParseSourceReject("duplicate source unknown key",
                      RootDoc("2", Q("t"),
                              "{\"type\":\"http\",\"profile\":\"demo\","
                              "\"SECRET9f3\":1,\"SECRET9f3\":2}",
                              Q("rows"), rows),
                      L"重复键");
    ParseSourceReject("duplicate row label",
                      V2RowsDoc(Arr({Obj({{"label", Q("l")}, {"label", Q("l")},
                                          {"text_path", Q("/x")}})})),
                      L"重复键");
    ParseSourceReject("duplicate row escaped text_path",
                      V2RowsDoc(Arr({Obj({{"label", Q("l")}, {"text_path", Q("/x")},
                                          {"text_pat\\u0068", Q("/y")}})})),
                      L"重复键");
    ParseSourceReject("duplicate bar max",
                      V2BarDoc(Arr({Obj({{"label", Q("l")}, {"value_path", Q("/v")},
                                         {"max", "2"}, {"max", "3"}})})),
                      L"重复键");
    ParseSourceReject("duplicate nested unknown",
                      V2RowsDoc(Arr({Obj({{"label", Q("l")}, {"text_path", Q("/x")},
                                          {"extra", "{\"a\":1,\"a\":2}"}})})),
                      L"重复键");
}

// --- RFC 6901 validation ------------------------------------------------

void TestPointerValidation() {
    const std::string body = Obj({{"x", Q("ok")}});

    DecodeReject("pointer no leading slash", RowsSpec("x"), body, L"Pointer");
    DecodeReject("pointer script syntax", RowsSpec("$..x"), body, L"Pointer");
    DecodeReject("pointer invalid tilde escape", RowsSpec("/a~2"), body, L"Pointer");
    DecodeReject("pointer trailing tilde", RowsSpec("/a~"), body, L"Pointer");
    DecodeReject("pointer over 256 bytes",
                 RowsSpec("/" + Repeat('a', cardspec::kMaxPointerBytes)),
                 Obj({{"x", Q("ok")}}), L"256");
    DecodeReject("pointer over 8 segments",
                 RowsSpec("/a/a/a/a/a/a/a/a/a"),
                 Obj({{"a", "1"}}), L"8");
    DecodeReject("pointer invalid UTF-8",
                 RowsSpec(std::string("/") + std::string("\x80", 1)),
                 body, L"UTF-8");

    // A pointer is not a URL, JSONPath expression or script.
    DecodeReject("pointer never evaluates object as container",
                 RowsSpec("/a"), Obj({{"a", Obj({{"b", Q("x")}})}}), L"text");
    DecodeReject("pointer over non-container", RowsSpec("/a/b"),
                 Obj({{"a", "1"}}), L"text_path");

    // Wildcards are not special: '*' is an ordinary object key, but an array
    // index must still be canonical decimal.
    cardspec::Spec star;
    if (DecodeOk("pointer star is literal object key", RowsSpec("/*"),
                 Obj({{"*", Q("star")}}), &star)) {
        Check(star.rows[0].text == L"star", "star object key selected literally");
    }
    DecodeReject("pointer star is not an array wildcard", RowsSpec("/items/*"),
                 Obj({{"items", Arr({Q("a")})}}), L"text_path");
}

// --- RFC 6901 resolution and display primitives ------------------------

void TestPointerResolution() {
    cardspec::Spec root;
    if (DecodeOk("empty pointer selects root", RowsSpec(""), Q("root-text"), &root)) {
        Check(root.rows[0].text == L"root-text", "empty pointer root string");
    }

    cardspec::Spec empty_key;
    if (DecodeOk("single slash selects empty key", RowsSpec("/"),
                 Obj({{"", Q("empty-key")}}), &empty_key)) {
        Check(empty_key.rows[0].text == L"empty-key", "single slash empty key");
    }

    cardspec::Spec unicode;
    if (DecodeOk("unicode object key", RowsSpec(U8(L"/名称")),
                 Obj({{U8(L"名称"), Q(U8(L"值"))}}), &unicode)) {
        Check(unicode.rows[0].text == L"值", "unicode key selected");
    }

    cardspec::Spec escaped;
    if (DecodeOk("tilde escapes decode literally", RowsSpec("/a~1b~0c"),
                 Obj({{"a/b~c", Q("escaped")}}), &escaped)) {
        Check(escaped.rows[0].text == L"escaped", "~0/~1 unescaped");
    }

    cardspec::Spec array_zero;
    if (DecodeOk("canonical zero index", RowsSpec("/items/0"),
                 Obj({{"items", Arr({Q("first"), Q("second")})}}), &array_zero)) {
        Check(array_zero.rows[0].text == L"first", "zero index selected");
    }

    cardspec::Spec array_one;
    if (DecodeOk("canonical non-zero index", RowsSpec("/items/1"),
                 Obj({{"items", Arr({Q("first"), Q("second")})}}), &array_one)) {
        Check(array_one.rows[0].text == L"second", "non-zero index selected");
    }

    const std::string two_items = Obj({{"items", Arr({Q("a"), Q("b")})}});
    DecodeReject("array index leading zero", RowsSpec("/items/01"), two_items,
                 L"text_path", L"01");
    DecodeReject("array index plus sign", RowsSpec("/items/+1"), two_items,
                 L"text_path", L"+1");
    DecodeReject("array index negative", RowsSpec("/items/-1"), two_items,
                 L"text_path", L"-1");
    DecodeReject("array index fraction", RowsSpec("/items/1.0"), two_items,
                 L"text_path", L"1.0");
    DecodeReject("array index empty", RowsSpec("/items/"), two_items, L"text_path");
    DecodeReject("array index overflow", RowsSpec("/items/" + Repeat('9', 30)),
                 two_items, L"text_path", std::wstring(30, L'9'));
    DecodeReject("array index out of range", RowsSpec("/items/2"), two_items, L"text_path");
    DecodeReject("missing object key", RowsSpec("/SECRET9f3"),
                 Obj({{"other", "1"}}), L"text_path", L"SECRET9f3");
}

void TestDecodeRowsTypes() {
    cardspec::Spec string_row;
    if (DecodeOk("row string", RowsSpec("/v"), Obj({{"v", Q("text")}}), &string_row)) {
        Check(string_row.rows[0].text == L"text", "row string bound");
    }

    cardspec::Spec number_row;
    if (DecodeOk("row number", RowsSpec("/v"), Obj({{"v", "10"}}), &number_row)) {
        Check(number_row.rows[0].text == L"10", "row number formatted compactly");
    }
    if (DecodeOk("row fractional number", RowsSpec("/v"), Obj({{"v", "0.5"}}),
                 &number_row)) {
        Check(number_row.rows[0].text == L"0.5", "row fractional number formatted");
    }
    cardspec::Spec bool_true;
    if (DecodeOk("row true", RowsSpec("/v"), Obj({{"v", "true"}}), &bool_true)) {
        Check(bool_true.rows[0].text == L"true", "row true bound");
    }
    cardspec::Spec bool_false;
    if (DecodeOk("row false", RowsSpec("/v"), Obj({{"v", "false"}}), &bool_false)) {
        Check(bool_false.rows[0].text == L"false", "row false bound");
    }

    // Unused fields may legitimately contain multiline strings, escaped
    // controls, objects, arrays and null: only the selected display text is
    // validated as text.
    cardspec::Spec unused;
    if (DecodeOk("unused fields do not poison the response", RowsSpec("/v"),
                 Obj({{"v", Q("ok")},
                      {"unused_multiline", Q("line1\\nline2")},
                      {"unused_control", Q("\\u0001")},
                      {"unused_object", Obj({{"a", "1"}})},
                      {"unused_array", Arr({Q("x")})},
                      {"unused_null", "null"}}),
                 &unused)) {
        Check(unused.rows[0].text == L"ok", "selected text bound despite unused fields");
    }

    DecodeReject("row null", RowsSpec("/v"), Obj({{"v", "null"}}), L"text");
    DecodeReject("row object", RowsSpec("/v"), Obj({{"v", Obj({{"a", "1"}})}}), L"text");
    DecodeReject("row array", RowsSpec("/v"), Obj({{"v", Arr({Q("x")})}}), L"text");
    DecodeReject("row infinite number", RowsSpec("/v"), Obj({{"v", "1e999"}}), L"有限");
    DecodeReject("row selected newline", RowsSpec("/v"),
                 Obj({{"v", Q("line1\\nline2")}}), L"控制字符");
    DecodeReject("row selected NUL", RowsSpec("/v"), Obj({{"v", Q("\\u0000")}}),
                 L"控制字符");
    DecodeReject("row selected DEL", RowsSpec("/v"), Obj({{"v", Q("\\u007f")}}),
                 L"控制字符");
    DecodeReject("row selected text too long", RowsSpec("/v"),
                 Obj({{"v", Q(Repeat('t', 97))}}), L"root.rows[0].text");
    DecodeReject("row selected lone surrogate escape", RowsSpec("/v"),
                 Obj({{"v", Q("\\ud800")}}), L"代理");
    DecodeReject("response duplicate key", RowsSpec("/v"),
                 "{\"v\":1,\"v\":2}", L"重复键");
    DecodeReject("response invalid UTF-8", RowsSpec("/v"),
                 std::string("{\"v\":\"\x80\"}"), L"UTF-8");
    DecodeReject("response oversized", RowsSpec("/v"),
                 "{\"v\":\"" + Repeat('a', cardspec::kMaxSpecBytes) + "\"}", L"32768");
}

// --- bar binding --------------------------------------------------------

void TestDecodeBar() {
    cardspec::Spec constant;
    if (DecodeOk("bar constant max", BarConstSpec("/done", 5.0),
                 Obj({{"done", "3"}}), &constant)) {
        Check(constant.rows[0].value == 3.0 && constant.rows[0].max == 5.0,
              "bar constant max bound");
    }

    cardspec::Spec pointer;
    if (DecodeOk("bar pointer max", BarPointerSpec("/done", "/total"),
                 Obj({{"done", "3"}, {"total", "5"}}), &pointer)) {
        Check(pointer.rows[0].value == 3.0 && pointer.rows[0].max == 5.0,
              "bar pointer max bound");
    }

    cardspec::Spec zero;
    if (DecodeOk("bar zero value", BarConstSpec("/done", 5.0),
                 Obj({{"done", "0"}}), &zero)) {
        Check(zero.rows[0].value == 0.0, "bar zero bound");
    }
    cardspec::Spec equal;
    if (DecodeOk("bar value equals max", BarConstSpec("/done", 5.0),
                 Obj({{"done", "5"}}), &equal)) {
        Check(equal.rows[0].value == 5.0, "bar value equals max accepted");
    }

    DecodeReject("bar string is not a number", BarConstSpec("/done", 5.0),
                 Obj({{"done", Q("3")}}), L"JSON 数值");
    DecodeReject("bar bool is not a number", BarConstSpec("/done", 5.0),
                 Obj({{"done", "true"}}), L"JSON 数值");
    DecodeReject("bar null is not a number", BarConstSpec("/done", 5.0),
                 Obj({{"done", "null"}}), L"JSON 数值");
    DecodeReject("bar array is not a number", BarConstSpec("/done", 5.0),
                 Obj({{"done", Arr({"1"})}}), L"JSON 数值");
    DecodeReject("bar object is not a number", BarConstSpec("/done", 5.0),
                 Obj({{"done", Obj({{"x", "1"}})}}), L"JSON 数值");
    DecodeReject("bar infinite value", BarConstSpec("/done", 5.0),
                 Obj({{"done", "1e999"}}), L"有限");
    DecodeReject("bar negative value", BarConstSpec("/done", 5.0),
                 Obj({{"done", "-1"}}), L"0..max");
    DecodeReject("bar value above max", BarConstSpec("/done", 2.0),
                 Obj({{"done", "3"}}), L"0..max");

    DecodeReject("bar max pointer string", BarPointerSpec("/done", "/total"),
                 Obj({{"done", "3"}, {"total", Q("5")}}), L"JSON 数值");
    DecodeReject("bar max pointer bool", BarPointerSpec("/done", "/total"),
                 Obj({{"done", "3"}, {"total", "true"}}), L"JSON 数值");
    DecodeReject("bar max pointer null", BarPointerSpec("/done", "/total"),
                 Obj({{"done", "3"}, {"total", "null"}}), L"JSON 数值");
    DecodeReject("bar max pointer infinite", BarPointerSpec("/done", "/total"),
                 Obj({{"done", "3"}, {"total", "1e999"}}), L"有限");
    DecodeReject("bar max pointer zero", BarPointerSpec("/done", "/total"),
                 Obj({{"done", "0"}, {"total", "0"}}), L"必须大于 0");
    DecodeReject("bar max pointer negative", BarPointerSpec("/done", "/total"),
                 Obj({{"done", "0"}, {"total", "-1"}}), L"必须大于 0");
    DecodeReject("bar max pointer range", BarPointerSpec("/done", "/total"),
                 Obj({{"done", "3"}, {"total", "2"}}), L"0..max");
    DecodeReject("bar value pointer missing", BarConstSpec("/SECRET9f3", 5.0),
                 Obj({{"other", "1"}}), L"value_path", L"SECRET9f3");
    DecodeReject("bar non-canonical array value", BarConstSpec("/values/01", 5.0),
                 Obj({{"values", Arr({"1"})}}), L"value_path", L"01");
}

// --- typed ValidateHttpSpec --------------------------------------------

void TestValidateHttpSpec() {
    {
        std::wstring error = L"stale error";
        Check(cardspec::ValidateHttpSpec(RowsSpec("/x"), &error),
              "valid rows http spec accepted");
        Check(error.empty(), "ValidateHttpSpec success clears error");
        error = L"stale error";
        Check(cardspec::ValidateHttpSpec(BarConstSpec("/x", 2.0), &error),
              "valid bar http spec accepted");
        Check(error.empty(), "ValidateHttpSpec bar success clears error");
        error = L"stale error";
        Check(cardspec::ValidateHttpSpec(BarPointerSpec("/x", "/m"), &error),
              "valid bar pointer http spec accepted");
        Check(error.empty(), "ValidateHttpSpec pointer success clears error");
    }

    {
        cardspec::HttpSpec spec = RowsSpec("/x");
        spec.profile.clear();
        std::wstring error;
        Check(!cardspec::ValidateHttpSpec(spec, &error), "empty profile rejected");
        Check(error.find(L"root.source.profile") != std::wstring::npos,
              "empty profile error path");
    }
    {
        cardspec::HttpSpec spec = RowsSpec("/x");
        spec.layout = static_cast<cardspec::Layout>(42);
        std::wstring error;
        Check(!cardspec::ValidateHttpSpec(spec, &error), "invalid layout enum rejected");
        Check(error.find(L"布局枚举") != std::wstring::npos,
              "invalid layout error path");
    }
    {
        cardspec::HttpSpec spec = RowsSpec("/x");
        spec.title.clear();
        std::wstring error;
        Check(!cardspec::ValidateHttpSpec(spec, &error), "empty title rejected");
        Check(error.find(L"root.title") != std::wstring::npos, "empty title error path");
    }
    {
        cardspec::HttpSpec spec = RowsSpec("/x");
        spec.title = L"a\x01b";
        std::wstring error;
        Check(!cardspec::ValidateHttpSpec(spec, &error), "control title rejected");
        Check(error.find(L"控制字符") != std::wstring::npos, "control title error");
    }
    {
        cardspec::HttpSpec spec = RowsSpec("/x");
        spec.rows.clear();
        std::wstring error;
        Check(!cardspec::ValidateHttpSpec(spec, &error), "empty rows rejected");
        Check(error.find(L"root.rows") != std::wstring::npos, "empty rows error path");
    }
    {
        cardspec::HttpSpec spec = RowsSpec("/x");
        const cardspec::HttpRow row = spec.rows[0];
        spec.rows.assign(9, row);
        std::wstring error;
        Check(!cardspec::ValidateHttpSpec(spec, &error), "nine rows rejected");
        Check(error.find(L"root.rows") != std::wstring::npos, "nine rows error path");
    }
    {
        cardspec::HttpSpec spec = RowsSpec("/x");
        spec.rows[0].label.clear();
        std::wstring error;
        Check(!cardspec::ValidateHttpSpec(spec, &error), "empty label rejected");
        Check(error.find(L"root.rows[0].label") != std::wstring::npos,
              "empty label error path");
    }
    {
        cardspec::HttpSpec spec = RowsSpec("/x");
        spec.rows[0].label = L"a\x7f" L"b";
        std::wstring error;
        Check(!cardspec::ValidateHttpSpec(spec, &error), "DEL label rejected");
        Check(error.find(L"控制字符") != std::wstring::npos, "DEL label error");
    }
    {
        cardspec::HttpSpec spec = RowsSpec("x");
        std::wstring error;
        Check(!cardspec::ValidateHttpSpec(spec, &error), "bad rows pointer rejected");
        Check(error.find(L"root.rows[0].text_path") != std::wstring::npos,
              "bad rows pointer error path");
    }
    {
        cardspec::HttpSpec spec = BarConstSpec("/x", 0.0);
        std::wstring error;
        Check(!cardspec::ValidateHttpSpec(spec, &error), "zero constant max rejected");
        Check(error.find(L"必须大于 0") != std::wstring::npos, "zero max error");
    }
    {
        cardspec::HttpSpec spec = BarConstSpec("/x", -1.0);
        std::wstring error;
        Check(!cardspec::ValidateHttpSpec(spec, &error), "negative constant max rejected");
        Check(error.find(L"必须大于 0") != std::wstring::npos, "negative max error");
    }
    {
        cardspec::HttpSpec spec = BarConstSpec(
            "/x", std::numeric_limits<double>::infinity());
        std::wstring error;
        Check(!cardspec::ValidateHttpSpec(spec, &error), "infinite constant max rejected");
        Check(error.find(L"有限") != std::wstring::npos, "infinite max error");
    }
    {
        cardspec::HttpSpec spec = BarConstSpec(
            "/x", std::numeric_limits<double>::quiet_NaN());
        std::wstring error;
        Check(!cardspec::ValidateHttpSpec(spec, &error), "NaN constant max rejected");
        Check(error.find(L"有限") != std::wstring::npos, "NaN max error");
    }
    {
        cardspec::HttpSpec spec = BarPointerSpec("x", "/m");
        std::wstring error;
        Check(!cardspec::ValidateHttpSpec(spec, &error), "bad bar value pointer rejected");
        Check(error.find(L"root.rows[0].value_path") != std::wstring::npos,
              "bad bar value pointer error path");
    }
    {
        cardspec::HttpSpec spec = BarPointerSpec("/x", "m");
        std::wstring error;
        Check(!cardspec::ValidateHttpSpec(spec, &error), "bad bar max pointer rejected");
        Check(error.find(L"root.rows[0].max_path") != std::wstring::npos,
              "bad bar max pointer error path");
    }
}

} // namespace

int wmain() {
    TestV1Compatibility();
    TestStrictJsonFacade();
    TestV2Legal();
    TestV2RootAndSourceErrors();
    TestV2RowsAndBarErrors();
    TestDuplicateKeys();
    TestPointerValidation();
    TestPointerResolution();
    TestDecodeRowsTypes();
    TestDecodeBar();
    TestValidateHttpSpec();

    std::printf("[cardspec-source-test] %s\n", failures == 0 ? "ok" : "failed");
    return failures == 0 ? 0 : 1;
}
