#include "help.h"

#include "cli.h"

namespace srv {
namespace {

void Block(Output& out, const std::wstring& text) {
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t nl = text.find(L'\n', start);
        if (nl == std::wstring::npos) {
            out.Line(text.substr(start));
            break;
        }
        out.Line(text.substr(start, nl - start));
        start = nl + 1;
    }
}

std::wstring General() {
    return std::wstring(L"srvctl ") + kVersion +
        L" - remote Linux server status and SSH commands\n"
        L"\n"
        L"USAGE\n"
        L"  srvctl [OPTIONS] COMMAND [ARGS...]\n"
        L"\n"
        L"COMMANDS\n"
        L"  status [HOST...]       CPU / memory / disk / load / uptime\n"
        L"  exec HOST [--] CMD...  Run a command on HOST over SSH\n"
        L"  ssh [HOST] [CMD...]    Open a login shell (or run CMD; no HOST = picker)\n"
        L"  hosts list             Show the configured hosts\n"
        L"  hosts add TARGET...    Add hosts (comma or space separated)\n"
        L"  hosts set TARGET...    Replace the host list\n"
        L"  hosts remove TARGET... Remove hosts\n"
        L"  hosts clear            Remove every host\n"
        L"  doctor [HOST...]       Diagnose ssh.exe, config and connectivity\n"
        L"  help [COMMAND]         Show help for a command\n"
        L"  version                Show the version\n"
        L"\n"
        L"OPTIONS\n"
        L"  -c, --config PATH      Config file (default: srvctl.ini next to srvctl.exe)\n"
        L"      --ssh PATH         ssh.exe to use\n"
        L"  -i, --key PATH         Private key, passed to ssh as -i\n"
        L"  -t, --timeout SEC      Probe timeout (default 8) or exec/ssh timeout\n"
        L"      --json             JSON output (status, hosts); NDJSON with --watch\n"
        L"      --color MODE       auto, always, never (default auto)\n"
        L"      --no-color         Same as --color never\n"
        L"  -w, --watch [SEC]      Refresh status every SEC seconds (default 5)\n"
        L"  -v, --verbose          Print the effective ssh command\n"
        L"      --tty              Force a pseudo-terminal for exec/ssh\n"
        L"  -h, --help             Show help\n"
        L"  -V, --version          Show the version\n"
        L"\n"
        L"EXAMPLES\n"
        L"  srvctl status\n"
        L"  srvctl status web-01 db-01 --json\n"
        L"  srvctl status --watch 5\n"
        L"  srvctl exec web-01 -- uptime\n"
        L"  srvctl ssh web-01\n"
        L"  srvctl ssh              rem pick from configured hosts\n"
        L"  srvctl hosts add web-01 db-01\n"
        L"\n"
        L"EXIT CODES\n"
        L"  0    success\n"
        L"  1    failure (all probes failed, or a remote command failed)\n"
        L"  2    usage error\n"
        L"  3    partial probe failure\n"
        L"  4    no hosts, or config / ssh client missing\n"
        L"  124  exec/ssh timeout\n"
        L"\n"
        L"CONFIG\n"
        L"  Double-clicking srvctl.exe opens SSH: one host connects directly,\n"
        L"  several hosts show a numbered picker.\n"
        L"  srvctl.ini is UTF-16 key=value: hosts, ssh, key, timeout.\n"
        L"  If srvctl.ini is absent, srvctl reads server.ini next to the\n"
        L"  executables (the widget config) and only writes srvctl.ini, so the\n"
        L"  widget's own settings are never changed by accident.\n"
        L"  Use '--config PATH' or the SRVCTL_CONFIG environment variable.\n";
}

std::wstring StatusHelp() {
    return L"srvctl status [HOST...] [OPTIONS]\n"
           L"\n"
           L"Probes each host over SSH and prints CPU, memory, root disk, 1-minute\n"
           L"load, uptime and round-trip time. With no HOST, uses hosts= from the\n"
           L"config. Probes run in parallel; one dead host costs one timeout.\n"
           L"\n"
           L"OPTIONS\n"
           L"  -w, --watch [SEC]  Refresh every SEC seconds (default 5). Ctrl+C stops.\n"
           L"      --json         One JSON object; with --watch, one object per line.\n"
           L"  -t, --timeout SEC  Probe timeout (default from config, 8).\n"
           L"      --ssh PATH     Override ssh.exe.\n"
           L"  -i, --key PATH     Override the private key.\n"
           L"  -c, --config PATH  Override the config file.\n"
           L"      --color MODE   auto, always, never.\n"
           L"\n"
           L"EXIT CODES\n"
           L"  0 all hosts up, 1 all failed, 3 some failed, 4 no hosts / no ssh.\n"
           L"\n"
           L"EXAMPLES\n"
           L"  srvctl status\n"
           L"  srvctl status web-01 root@10.0.0.5\n"
           L"  srvctl status --watch 5 --timeout 3\n"
           L"  srvctl status --json | jq '.hosts[] | select(.ok == false)'\n";
}

std::wstring ExecHelp() {
    return L"srvctl exec HOST [OPTIONS] [--] COMMAND...\n"
           L"\n"
           L"Runs COMMAND through the remote login shell over ssh. Output streams to\n"
           L"this console and ssh's exit status becomes srvctl's exit status, so it\n"
           L"composes in scripts. The command is not run through a local shell.\n"
           L"\n"
           L"Options must come before HOST; everything after HOST is sent to the\n"
           L"remote side. Put '--' after HOST when the first word would otherwise\n"
           L"look like a local option.\n"
           L"\n"
           L"OPTIONS\n"
           L"  -t, --timeout SEC  Kill ssh after SEC seconds (default: no limit).\n"
           L"      --tty          Request a pseudo-terminal (interactive programs).\n"
           L"  -v, --verbose      Print the effective ssh command to stderr.\n"
           L"      --ssh PATH / -i KEY / -c PATH  Local ssh overrides.\n"
           L"\n"
           L"EXAMPLES\n"
           L"  srvctl exec web-01 -- uname -a\n"
           L"  srvctl exec web-01 \"systemctl status nginx --no-pager\"\n"
           L"  srvctl exec --timeout 30 web-01 -- tail -n 50 /var/log/syslog\n"
           L"  srvctl exec --tty db-01 -- htop\n"
           L"\n"
           L"NOTE\n"
           L"  ssh joins command words with spaces before handing them to the remote\n"
           L"  shell. Quote compound commands as one argument:\n"
           L"    srvctl exec web-01 \"for f in /etc/*.conf; do echo $f; done\"\n"
           L"  A timeout kills the local ssh process; a remote command that ignored\n"
           L"  SIGHUP may keep running. Use the remote's own timeout for hard limits.\n";
}

std::wstring SshHelp() {
    return L"srvctl ssh [HOST] [CMD...]\n"
           L"\n"
           L"Opens an interactive login shell on HOST, or runs CMD there. This is a\n"
           L"thin wrapper around ssh with the same key/config overrides as the rest\n"
           L"of srvctl; it does not capture output, so password prompts and full-\n"
           L"screen programs work normally.\n"
           L"\n"
           L"With no HOST, a single configured host is used directly; several hosts\n"
           L"show a numbered picker. Double-clicking srvctl.exe uses this same path.\n"
           L"\n"
           L"OPTIONS (before HOST)\n"
           L"      --tty          Force a pseudo-terminal (-t).\n"
           L"  -t, --timeout SEC  Kill ssh after SEC seconds (default: no limit).\n"
           L"  -v, --verbose      Print the effective ssh command to stderr.\n"
           L"      --ssh PATH / -i KEY / -c PATH  Local ssh overrides.\n"
           L"\n"
           L"EXAMPLES\n"
           L"  srvctl ssh\n"
           L"  srvctl ssh web-01\n"
           L"  srvctl ssh web-01 \"tmux attach || tmux new\"\n";
}

std::wstring HostsHelp() {
    return L"srvctl hosts ACTION [TARGET...]\n"
           L"\n"
           L"Manages the host list in srvctl.ini. Targets are SSH config aliases or\n"
           L"user@host[:port] strings, separated by spaces or commas. Up to 32 are\n"
           L"stored; the desktop widget displays the first 8.\n"
           L"\n"
           L"ACTIONS\n"
           L"  list                 Show config path, ssh client and hosts (default).\n"
           L"  add TARGET...        Append targets, skipping duplicates.\n"
           L"  set TARGET...        Replace the whole list.\n"
           L"  remove TARGET...     Remove by target or unambiguous display name.\n"
           L"  clear                Remove every host.\n"
           L"\n"
           L"OPTIONS\n"
           L"      --json           JSON output for 'list'.\n"
           L"  -c, --config PATH    Edit a specific file.\n"
           L"\n"
           L"EXAMPLES\n"
           L"  srvctl hosts\n"
           L"  srvctl hosts add web-01 db-01 root@10.0.0.5\n"
           L"  srvctl hosts remove db-01\n"
           L"  srvctl hosts list --json\n"
           L"\n"
           L"NOTE\n"
           L"  When srvctl.ini does not exist, the list is inherited from server.ini\n"
           L"  (the widget). The first edit creates srvctl.ini next to srvctl.exe and\n"
           L"  seeds it with the inherited values; server.ini is never rewritten.\n";
}

std::wstring DoctorHelp() {
    return L"srvctl doctor [HOST...]\n"
           L"\n"
           L"Checks the local OpenSSH client, the config file, the ~/.ssh directory\n"
           L"and real connectivity to each host, then prints one actionable hint per\n"
           L"failure. Uses the same probe engine as 'status'.\n"
           L"\n"
           L"OPTIONS\n"
           L"  -t, --timeout SEC  Probe timeout (default from config, 8).\n"
           L"      --ssh PATH / -i KEY / -c PATH  Local ssh overrides.\n"
           L"\n"
           L"EXIT CODES\n"
           L"  0 all checks and probes passed, 1 anything failed.\n"
           L"\n"
           L"EXAMPLES\n"
           L"  srvctl doctor\n"
           L"  srvctl doctor web-01\n";
}

} // namespace

void PrintGeneralHelp(Output& out) {
    Block(out, General());
}

void PrintCommandHelp(const std::wstring& command, Output& out) {
    if (command == L"status") Block(out, StatusHelp());
    else if (command == L"exec") Block(out, ExecHelp());
    else if (command == L"ssh") Block(out, SshHelp());
    else if (command == L"hosts") Block(out, HostsHelp());
    else if (command == L"doctor") Block(out, DoctorHelp());
    else Block(out, General());
}

} // namespace srv
