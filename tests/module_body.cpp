#include "widget/module.h"

#include <climits>
#include <cstdio>
#include <cwchar>

namespace {

int failures = 0;

void Check(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wprintf(L"[module-body-test] FAIL %ls\n", message);
        ++failures;
    }
}

// Minimal Module implementation: everything the contract requires, no host.
class DummyModule final : public wg::Module {
public:
    wg::ModuleSpec Describe() const override { return {}; }
    void Load() override {}
    void Refresh() override {}
    void Commit() override {}
    int BodyHeight() const override { return 120; }
    void PaintBody(wg::Canvas&, const wg::Rect&) override {}
};

// ModuleT implementation for the data-buffer defaults.
struct DummyData {
    int height = 64;
};

class DummyModuleT final : public wg::ModuleT<DummyData> {
public:
    wg::ModuleSpec Describe() const override { return {}; }

protected:
    void RefreshData(DummyData&) override {}
    int BodyHeightOf(const DummyData& data) const override { return data.height; }
    void PaintBodyOf(const DummyData&, wg::Canvas&, const wg::Rect&) override {}
};

} // namespace

int wmain() {
    using namespace wg;

    // ---- Module defaults for the body-size contract ------------------------
    {
        DummyModule module;
        Check(module.MinBodyHeight() == 0, L"Module defaults MinBodyHeight to 0");
        Check(module.MaxBodyHeight() == INT_MAX, L"Module defaults MaxBodyHeight to INT_MAX");
        Check(module.BodyHeight() == 120 && module.PendingBodyHeight() == 120,
              L"the default PendingBodyHeight mirrors BodyHeight");
        // Default lifecycle hooks are callable no-ops.
        module.OnBodySizeChanged(Rect{1, 2, 3, 4});
        module.OnExpandChanged(false);
        Check(true, L"default OnBodySizeChanged/OnExpandChanged are no-ops");
    }
    {
        DummyModuleT module;
        Check(module.MinBodyHeight() == 0, L"ModuleT keeps the MinBodyHeight default");
        Check(module.MaxBodyHeight() == INT_MAX, L"ModuleT keeps the MaxBodyHeight default");
        Check(module.BodyHeight() == 64 && module.PendingBodyHeight() == 64,
              L"ModuleT BodyHeight defaults to the shown data");
    }

    // ---- ClampBodyHeight ---------------------------------------------------
    bool exceeded = true;
    exceeded = true;
    Check(ClampBodyHeight(100, 0, 200, &exceeded) == 100 && !exceeded,
          L"a height inside the range is unchanged");
    exceeded = true;
    Check(ClampBodyHeight(0, 0, 200, &exceeded) == 0 && !exceeded,
          L"the minimum bound is inclusive");
    exceeded = true;
    Check(ClampBodyHeight(200, 0, 200, &exceeded) == 200 && !exceeded,
          L"the maximum bound is inclusive");
    exceeded = true;
    Check(ClampBodyHeight(50, 80, 200, &exceeded) == 80 && !exceeded,
          L"a height below the minimum is raised");
    exceeded = false;
    Check(ClampBodyHeight(300, 0, 200, &exceeded) == 200 && exceeded,
          L"a height above the maximum clamps and reports exceededMax");
    exceeded = true;
    Check(ClampBodyHeight(300, 0, 200, nullptr) == 200,
          L"exceededMax may be null");
    exceeded = true;
    Check(ClampBodyHeight(-5, -10, 100, &exceeded) == 0 && !exceeded,
          L"a negative minimum is treated as 0");
    exceeded = true;
    Check(ClampBodyHeight(50, -10, 100, &exceeded) == 50 && !exceeded,
          L"a negative minimum does not raise a valid height");
    exceeded = true;
    Check(ClampBodyHeight(300, 50, -1, &exceeded) == 300 && !exceeded,
          L"a negative maximum is ignored (only the minimum applies)");
    exceeded = true;
    Check(ClampBodyHeight(40, 50, -1, &exceeded) == 50 && !exceeded,
          L"a negative maximum still applies the minimum");
    exceeded = true;
    Check(ClampBodyHeight(300, 100, 50, &exceeded) == 300 && !exceeded,
          L"an inverted maximum is ignored (only the minimum applies)");
    exceeded = true;
    Check(ClampBodyHeight(40, 100, 50, &exceeded) == 100 && !exceeded,
          L"an inverted maximum still applies the minimum");
    exceeded = true;
    Check(ClampBodyHeight(-20, 0, 100, &exceeded) == 0 && !exceeded,
          L"a negative request is raised to the minimum");
    exceeded = true;
    Check(ClampBodyHeight(-20, -5, 100, &exceeded) == 0 && !exceeded,
          L"a negative request with a negative minimum becomes 0");
    exceeded = true;
    Check(ClampBodyHeight(INT_MAX, 0, INT_MAX, &exceeded) == INT_MAX && !exceeded,
          L"the default INT_MAX maximum accepts any height");
    exceeded = true;
    Check(ClampBodyHeight(10, 50, -1, &exceeded) == 50 && !exceeded,
          L"minimum-only clamping leaves exceededMax false");

    if (failures) {
        std::wprintf(L"[module-body-test] %d failure(s)\n", failures);
        return 1;
    }
    std::wprintf(L"[module-body-test] ok\n");
    return 0;
}
