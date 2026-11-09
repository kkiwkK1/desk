#pragma once

#include "cli.h"
#include "config.h"
#include "output.h"

namespace srv {

int CmdStatus(const Options& options, const Config& config, Output& out);
int CmdExec(const Options& options, const Config& config, Output& out);
int CmdSsh(const Options& options, const Config& config, Output& out);
int CmdHosts(const Options& options, Config& config, Output& out);
int CmdDoctor(const Options& options, const Config& config, Output& out);

} // namespace srv
