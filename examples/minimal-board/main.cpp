// A separate consumer of the framework: no desk, data or panels headers.
#include "counter.h"

#include "widget/board.h"

#include <memory>
#include <vector>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    std::vector<std::unique_ptr<wg::Module>> modules;
    modules.push_back(CreateClockPanel());

    wg::BoardOptions options;
    options.id = L"sample-board";
    options.title = L"Sample Board";
    options.boardTitle = L"SAMPLE BOARD";
    options.startWithWindows = false;
    return wg::RunBoard(std::move(modules), options);
}
