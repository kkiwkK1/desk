// Example widget: the smallest useful thing on top of the framework.
// No network, no settings, one data struct and four small functions.
#include "widget/host.h"

#include <cwchar>
#include <memory>

namespace {

struct ClockData {
    std::wstring time;
    std::wstring date;
};

class Clock : public wg::WidgetT<ClockData> {
public:
    wg::Desc Describe() override { return wg::Desc{L"clock", L"Desktop Clock", 220}; }

protected:
    void LoadData(ClockData& data) override { Now(data); }
    void RefreshData(ClockData& data) override { Now(data); }
    int HeightOf(const ClockData&) const override { return 78; }

    void PaintData(const ClockData& data, wg::Canvas& canvas) override {
        canvas.Panel(theme_, editing());
        canvas.TextCentered(data.time, canvas.width() / 2, 10.0f, theme_.text, 3.0f);
        canvas.TextCentered(data.date, canvas.width() / 2, 52.0f, theme_.faint, 1.0f);
    }

private:
    static void Now(ClockData& data) {
        SYSTEMTIME t{};
        GetLocalTime(&t);

        wchar_t time[16];
        swprintf(time, 16, L"%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
        data.time = time;

        static const wchar_t* kDays[] = {L"SUN", L"MON", L"TUE", L"WED", L"THU", L"FRI", L"SAT"};
        static const wchar_t* kMonths[] = {L"JAN", L"FEB", L"MAR", L"APR", L"MAY", L"JUN",
                                           L"JUL", L"AUG", L"SEP", L"OCT", L"NOV", L"DEC"};
        wchar_t date[32];
        swprintf(date, 32, L"%ls %02d %ls %04d", kDays[t.wDayOfWeek], t.wDay,
                 kMonths[t.wMonth - 1], t.wYear);
        data.date = date;
    }

    wg::Theme theme_;
};

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wg::Options options;
    options.refreshSeconds = 1;
    options.startWithWindows = false;
    return wg::Run(std::make_unique<Clock>(), options);
}
