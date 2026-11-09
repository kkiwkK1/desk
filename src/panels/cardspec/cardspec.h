// CARDSPEC card: one fixed, declarative card from <config dir>\cardspec.json.
// v1 inline is local; v2 HTTP only references a separately authorized profile
// and binds an injected fetch result. No arbitrary path or code generation.
// The host owns the worker; the module has no timer (refreshSeconds = 0).
// The host refreshes once after OnMenu: menu callbacks must not also call
// RequestRefresh(), which would queue a duplicate fetch. Missing / oversized /
// invalid input replaces the old data with a readable error; fixing the file
// and refreshing restores the content.
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
