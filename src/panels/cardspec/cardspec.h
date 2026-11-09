// CARDSPEC card (M2.0): one fixed, declarative card rendered from
// <config dir>\cardspec.json.
//
// The card owns no input surface: it reads that one fixed file inside the
// config directory, parses it through the frozen cardspec:: API and paints the
// spec title plus 1..8 rows (rows or bar layout). There is no timer, no
// thread, no network, no arbitrary path and no code generation: refresh is
// manual (refreshSeconds = 0) through the card menu, which only asks the host
// for a refresh (wg::Module::RequestRefresh). A missing / oversized / invalid
// file turns into a readable Chinese error card; fixing the file and
// refreshing restores the content.
//
// LoadData / RefreshData fully replace the staged view, so a failure can never
// publish stale successful data, and PaintBody only reads the host's shown
// snapshot (ModuleT staging/shown split).
#pragma once

#include "widget/module.h"
#include "data/cardspec/http.h"

#include <functional>
#include <memory>

// Test seams are native-only capabilities, never configurable by CardSpec.
struct CardSpecOptions {
    cardspec::HttpFetcher fetch;
    std::function<bool(const std::wstring&)> confirmNetwork;
};

std::unique_ptr<wg::Module> CreateCardSpecModule();
std::unique_ptr<wg::Module> CreateCardSpecModule(CardSpecOptions options);
