// Shared AiMeter data collection, used by both the standalone widget and the
// Account card. Keeping one function guarantees they report the same numbers.
#pragma once

#include "aimeter_data.h"

#include "widget/canvas.h"
#include "widget/store.h"

#include <vector>

namespace aimeter {

std::vector<wg::Row> InitialRows();
void Refresh(AimeterData& data, wg::Store& store, const wg::Theme& theme);

} // namespace aimeter
