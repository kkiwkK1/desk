#pragma once

#include "data/aimeter/aimeter_data.h"

#include "widget/canvas.h"
#include "widget/geom.h"
#include "widget/module.h"

namespace account_view {

int BodyHeight(const aimeter::AimeterData& data, bool expanded);
void DrawBody(const aimeter::AimeterData& data, wg::Canvas& canvas, const wg::Rect& body,
              const wg::Theme& theme, bool expanded);
wg::StatusChip Status(const aimeter::AimeterData& data, const wg::Theme& theme);

} // namespace account_view
