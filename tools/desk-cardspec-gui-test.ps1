# desk-cardspec-gui-test.ps1 - isolated CARDSPEC on-board GUI acceptance for M2.
#
# What this script proves (real Win32 GUI, real menu clicks, real MessageBox):
#   * Add module -> CARDSPEC only, the card lands on the board and is persisted;
#   * Chinese rows2/rows4 specs reflow (title + row ink bands, card height);
#   * editing cardspec.json does NOT auto-refresh; the card menu Reload applies it;
#   * bar 3/5 vs 0/5 pixel fill width;
#   * invalid / missing cardspec.json clears the old rows and shows a readable
#     error; fixing the file and reloading recovers WITHOUT a restart;
#   * v2 HTTP: unapproved -> BLOCKED with zero fetches; the real approve dialog
#     (Cancel / OK on the real MB_OKCANCEL box), grant persisted, fixed
#     synthetic response rendered as
#     HTTP + AS OF + row ink; every manual reload fetches exactly once; revoke clears the
#     grant and the old values; restart keeps grant/visibility and the initial
#     Load never fetches;
#   * negative controls: cardspec-only without an instance suffix, without
#     DESK_TEST_CONFIG_DIR and with an invalid DESK_TEST_CONFIG_DIR must all
#     exit 2 and own no window.
#
# What it deliberately CANNOT prove:
#   * real TLS/SNI/pin/peer/HTTP: every fetch in this mode is the fixed native
#     fake in src/apps/desk/main.cpp. A green run proves "real GUI + fake
#     transport", never network or TLS acceptance;
#   * the exact glyphs painted. The fixture body is asserted statically (the
#     copied image contains the fixed JSON literal) and structurally (HTTP chip
#     colour, row ink bands, AS OF band). There is no OCR in this harness.
#
# Contract notes read from the code (never "fix" the product to match a guess):
#   * docs/m2-http-plan.md section 3: "批准成功可 RequestRefresh" -> a successful
#     approval causes exactly ONE fetch. The fetch counter is asserted to grow by
#     exactly +1 for an approval and for every manual Reload, and to stay
#     unchanged (0 delta) when no grant exists (unapproved / cancelled / revoked).
#   * single dispatch is a product property, not a tolerance of this script: the
#     cardspec menu no longer calls RequestRefresh for approve/reload (the host
#     already starts one worker after OnMenu), so one menu action = one
#     transport entry. Observing 2 is a FAIL here, never a widened range.
#   * the approve confirmation is the real system #32770 MessageBox that
#     src/apps/desk/main.cpp injects for the cardspec-only fixture (the product
#     DefaultConfirm: MessageBoxW(nullptr, message, L"CARDSPEC · 网络授权",
#     MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2)). It is an OK/Cancel box, so
#     no button is pressed before the title is exactly "CARDSPEC · 网络授权",
#     both IDOK and IDCANCEL exist and DM_GETDEFID reports Cancel (id 2) as the
#     default; a mismatch is a FAIL and the box is closed without pressing.
#   * CARDSPEC declares SizePreset::Wide whose minHeight is 168, so a 1-2 row
#     card is 168 px tall even though its body only asks for 92 px. Height
#     checks use the framework value (body + 48, rounded up to 8, clamped to the
#     preset minimum) and never a hand-waved number.
#   * the own-canvas hook (src/widget/board.cpp, WM_APP+11 = 0x800B) writes
#     <DESK_TEST_CONFIG_DIR>\assistant-test-frame-<0|1>.bmp and never reads the
#     screen. This script uses only that hook: it never grabs the desktop and
#     never prints another process' window. If WM_APP+11 refuses (returns != 1)
#     the pixel checks are reported BLOCKED, never passed on a guess.
#
# Isolation (every item below is asserted or reported):
#   * one self-contained copy of build\bin\desk.exe under
#     build\qa\cardspec-gui\<run>\app, launched with its own
#     DESK_TEST_INSTANCE suffix, DESK_TEST_CONFIG_DIR, DESK_TEST_CARDSPEC_ONLY
#     and a private APPDATA/LOCALAPPDATA/TEMP/USERPROFILE. Those variables exist
#     only in the child environment; this script never mutates its own;
#   * every write path is refused unless it stays under the run directory;
#   * only PIDs this script started are stopped, and only after re-proving the
#     PID + creation time + image path; descendants are recorded before close;
#   * HKCU\...\Run values and the metadata fingerprint (names/sizes/mtimes only,
#     never contents) of the production config targets are compared before and
#     after. A target owned by a live production desk.exe is recorded but never
#     asserted;
#   * offline: no DNS, no HTTP, no credentials, no user data directory is read.
#
# Usage (the Lead decides when to build, sign and run it):
#   powershell -ExecutionPolicy Bypass -File tools\desk-cardspec-gui-test.ps1 -StaticOnly
#   powershell -ExecutionPolicy Bypass -File tools\desk-cardspec-gui-test.ps1
#
# Exit codes: 0 = every check passed, 1 = a check failed or is blocked,
#             2 = the script refused to run (missing/unsafe preconditions).

[CmdletBinding()]
param(
    [string]$ExePath = '',
    [int]$TimeoutSeconds = 60,
    [int]$SettleMs = 1000,
    [switch]$StaticOnly
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'desk-testlib.ps1')

$script:CanvasMessage = 0x800B
$script:RepoRoot = Split-Path -Parent $PSScriptRoot
$script:BuildRoot = Join-Path $script:RepoRoot 'build'
$script:QaRoot = Join-Path $script:BuildRoot 'qa\cardspec-gui'
$script:Run = $null
$script:Result = $null
$script:Boards = New-Object System.Collections.Generic.List[object]

# Fixed fixture contract (src/apps/desk/main.cpp cardspec-only branch).
$script:FixtureBody = '{"items":[{"name":"HTTP_FIXTURE_ROW","text":"SYNTHETIC_RESPONSE","value":3,"max":5}]}'
$script:CardWidth = 568
$script:CardHeaderH = 30
$script:CardPadTop = 8
$script:CardPadBottom = 10
$script:CardPadX = 12
$script:TitleBlock = 24
$script:RowPitch = 34
$script:BarOffsetY = 20
$script:BarH = 6
# Footer geometry from src/panels/cardspec/cardspec_view.cpp DrawBody:
#   as-of top = body.y + kTitleBlock + rows*kRowPitch + kAsOfGap, height kLineH.
$script:AsOfGap = 6
$script:LineH = 16
$script:WideMinHeight = 168
$script:MenuFetchExact = 1

# Theme colours (src/widget/canvas.h + skin.cpp). The canvas fills are exact, so
# a small tolerance is enough and no visual threshold has to be softened.
$script:ColorBorder = @(0x50, 0x55, 0x61)
$script:ColorOk = @(0x4A, 0xDE, 0x80)
$script:ColorWarn = @(0xFB, 0xBF, 0x24)
$script:ColorBad = @(0xF8, 0x71, 0x71)
$script:ColorTol = 20

# ---------------------------------------------------------------------------
# evidence framing: three-state checks (pass / fail / blocked)
# ---------------------------------------------------------------------------

function Add-CsCheck {
    # status: 'pass' | 'fail' | 'blocked'. A blocked check is never a pass: an
    # unavailable measurement (MSAA, dialog, pixel hook) must not read as green.
    param(
        [object]$Result,
        [string]$Name,
        [bool]$Pass,
        [string]$Detail = '',
        [ValidateSet('', 'pass', 'fail', 'blocked')][string]$Status = ''
    )
    $state = $Status
    if (-not $state) { if ($Pass) { $state = 'pass' } else { $state = 'fail' } }
    if ($state -ne 'pass') { $Pass = $false }
    $Result.Checks.Add([pscustomobject]@{ Name = $Name; Pass = $Pass; Status = $state; Detail = $Detail })
    Write-Host ('[{0}] {1}{2}' -f $state.ToUpperInvariant(), $Name, $(if ($Detail) { '  (' + $Detail + ')' } else { '' }))
    return $Pass
}

function Get-CsCounts {
    param([object]$Result)
    $pass = @($Result.Checks | Where-Object { $_.Status -eq 'pass' }).Count
    $fail = @($Result.Checks | Where-Object { $_.Status -eq 'fail' }).Count
    $blocked = @($Result.Checks | Where-Object { $_.Status -eq 'blocked' }).Count
    return [pscustomobject]@{ Pass = $pass; Fail = $fail; Blocked = $blocked; Total = $Result.Checks.Count }
}

function Save-CsResult {
    # PowerShell 5.1, UTF-8 without BOM, explicit arrays: every collection is
    # materialised with .ToArray() so a single-element list still serialises as
    # a JSON array and the evidence stays mechanically diffable. Name is a
    # parameter because a static-only report is not a GUI acceptance report.
    # This function must never throw: a failed run still has to leave a report.
    param([object]$Result, [string]$Path, [string]$Name = 'cardspec-gui', [hashtable]$Extra = @{})
    $json = ''
    $passed = $false
    try {
        $counts = Get-CsCounts -Result $Result
        $checks = @($Result.Checks | ForEach-Object {
            [pscustomobject]@{ name = $_.Name; status = $_.Status; pass = [bool]$_.Pass; detail = [string]$_.Detail }
        })
        $payload = [ordered]@{
            name     = $Name
            pass     = ($counts.Fail -eq 0 -and $counts.Blocked -eq 0)
            counts   = [pscustomobject]@{ pass = $counts.Pass; fail = $counts.Fail; blocked = $counts.Blocked; total = $counts.Total }
            at       = (Get-Date).ToString('s')
            checks   = [object[]]$checks
            notes    = [string[]]@($Result.Notes.ToArray())
            evidence = [string[]]@($Result.Evidence.ToArray())
        }
        if ($Extra -and $Extra.Count -gt 0) {
            foreach ($key in @($Extra.Keys)) { $payload[$key] = $Extra[$key] }
        }
        $passed = [bool]$payload.pass
        $json = ([pscustomobject]$payload) | ConvertTo-Json -Depth 12
    } catch {
        $passed = $false
        $json = ''
        Write-Host ('[cardspec-gui] report serialisation failed: ' + $_.Exception.Message) -ForegroundColor Yellow
    }
    if (-not $json) {
        $passCount = -1
        $failCount = -1
        $blockedCount = -1
        try {
            $counts = Get-CsCounts -Result $Result
            $passCount = $counts.Pass
            $failCount = $counts.Fail
            $blockedCount = $counts.Blocked
        } catch { }
        $json = '{"name":"' + $Name + '","pass":false,"counts":{"pass":' + $passCount +
            ',"fail":' + $failCount + ',"blocked":' + $blockedCount +
            '},"error":"report serialisation failed","at":"' + (Get-Date).ToString('s') + '"}'
        $passed = $false
    }
    try {
        $directory = Split-Path -Parent $Path
        if ($directory -and -not (Test-Path -LiteralPath $directory)) {
            New-Item -ItemType Directory -Path $directory -Force | Out-Null
        }
        [System.IO.File]::WriteAllText($Path, $json, (New-Object System.Text.UTF8Encoding($false)))
    } catch {
        Write-Host ('[cardspec-gui] cannot write the report: ' + $_.Exception.Message) -ForegroundColor Red
        return $false
    }
    return $passed
}

# ---------------------------------------------------------------------------
# native helpers (own types; desk-testlib is dot-sourced but never modified)
# ---------------------------------------------------------------------------

function Initialize-CsLaunch {
    # Same shape as the assistant harness launcher: DETACHED_PROCESS (no
    # inherited console), CREATE_UNICODE_ENVIRONMENT, an explicit child
    # environment, stdout/stderr captured as evidence, and a kept process handle
    # so a board that never shows a window can still be asked for its exit code.
    if ('DeskCardspecLaunch' -as [type]) { return }
    Add-Type -ReferencedAssemblies 'System' -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class DeskCardspecLaunch {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO {
        public int cb;
        public string lpReserved;
        public string lpDesktop;
        public string lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2;
        public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION {
        public IntPtr hProcess, hThread;
        public int dwProcessId, dwThreadId;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct SECURITY_ATTRIBUTES {
        public int nLength;
        public IntPtr lpSecurityDescriptor;
        public int bInheritHandle;
    }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool CreateProcess(string lpApplicationName, string lpCommandLine,
        IntPtr lpProcessAttributes, IntPtr lpThreadAttributes, bool bInheritHandles,
        uint dwCreationFlags, IntPtr lpEnvironment, string lpCurrentDirectory,
        ref STARTUPINFO si, out PROCESS_INFORMATION pi);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateFileW(string name, uint desiredAccess, uint shareMode,
        ref SECURITY_ATTRIBUTES attributes, uint creationDisposition, uint flagsAndAttributes,
        IntPtr templateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);

    [DllImport("kernel32.dll")]
    static extern IntPtr GetStdHandle(int which);

    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr handle);

    static readonly Dictionary<int, IntPtr> Tracked = new Dictionary<int, IntPtr>();
    static readonly object TrackedLock = new object();

    static IntPtr BuildEnvironment(string[] names, string[] values) {
        SortedDictionary<string, string> map = new SortedDictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (System.Collections.DictionaryEntry entry in Environment.GetEnvironmentVariables()) {
            string key = entry.Key as string;
            if (key == null) continue;
            map[key] = entry.Value as string ?? "";
        }
        for (int i = 0; i < names.Length && i < values.Length; ++i) {
            if (string.IsNullOrEmpty(names[i])) continue;
            map[names[i]] = values[i] ?? "";
        }
        string block = "";
        foreach (KeyValuePair<string, string> pair in map) block += pair.Key + "=" + pair.Value + "\0";
        block += "\0";
        byte[] bytes = Encoding.Unicode.GetBytes(block);
        IntPtr memory = Marshal.AllocHGlobal(bytes.Length);
        Marshal.Copy(bytes, 0, memory, bytes.Length);
        return memory;
    }

    public static int StartObserved(string exe, string workingDirectory, string[] names,
                                    string[] values, string stdoutPath, string stderrPath) {
        STARTUPINFO si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
        PROCESS_INFORMATION pi = new PROCESS_INFORMATION();
        // DETACHED_PROCESS (0x8) + CREATE_UNICODE_ENVIRONMENT (0x400) + CREATE_NEW_PROCESS_GROUP (0x200).
        const uint flags = 0x00000008u | 0x00000400u | 0x00000200u;
        SECURITY_ATTRIBUTES sa = new SECURITY_ATTRIBUTES();
        sa.nLength = Marshal.SizeOf(typeof(SECURITY_ATTRIBUTES));
        sa.bInheritHandle = 1;
        IntPtr outHandle = IntPtr.Zero;
        IntPtr errHandle = IntPtr.Zero;
        bool redirect = false;
        try {
            try {
                if (!string.IsNullOrEmpty(stdoutPath))
                    outHandle = CreateFileW(stdoutPath, 0x40000000u, 3u, ref sa, 2u, 0x80u, IntPtr.Zero);
                if (!string.IsNullOrEmpty(stderrPath))
                    errHandle = CreateFileW(stderrPath, 0x40000000u, 3u, ref sa, 2u, 0x80u, IntPtr.Zero);
                redirect = outHandle != IntPtr.Zero || errHandle != IntPtr.Zero;
            } catch {
                redirect = false;
            }
            if (redirect) {
                si.dwFlags = 0x00000100;
                si.hStdInput = GetStdHandle(-10);
                si.hStdOutput = outHandle != IntPtr.Zero ? outHandle : errHandle;
                si.hStdError = errHandle != IntPtr.Zero ? errHandle : outHandle;
            }
            IntPtr env = BuildEnvironment(names, values);
            try {
                string commandLine = "\"" + exe + "\"";
                if (!CreateProcess(exe, commandLine, IntPtr.Zero, IntPtr.Zero, redirect, flags, env,
                                   workingDirectory, ref si, out pi)) {
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                }
            } finally {
                Marshal.FreeHGlobal(env);
            }
        } finally {
            if (outHandle != IntPtr.Zero) CloseHandle(outHandle);
            if (errHandle != IntPtr.Zero) CloseHandle(errHandle);
        }
        CloseHandle(pi.hThread);
        lock (TrackedLock) { Tracked[pi.dwProcessId] = pi.hProcess; }
        return pi.dwProcessId;
    }

    public static bool TryGetExit(int pid, out bool exited, out int exitCode) {
        exited = false;
        exitCode = -1;
        IntPtr handle;
        lock (TrackedLock) {
            if (!Tracked.TryGetValue(pid, out handle)) return false;
        }
        uint code;
        if (!GetExitCodeProcess(handle, out code)) return false;
        exitCode = unchecked((int)code);
        exited = code != 259u;
        return true;
    }

    public static void ForgetTracked(int pid) {
        IntPtr handle;
        lock (TrackedLock) {
            if (!Tracked.TryGetValue(pid, out handle)) return;
            Tracked.Remove(pid);
            CloseHandle(handle);
        }
    }
}
'@ -ErrorAction Stop
}

function Initialize-CsMenu {
    # MSAA on a background STA thread: a popup command may open a modal
    # MessageBox on the board UI thread, so accDoDefaultAction must not run on
    # the thread that has to dismiss that dialog.
    if ('DeskCardspecMenu' -as [type]) { return $true }
    $accAssembly = [System.Reflection.Assembly]::LoadWithPartialName('Accessibility')
    if (-not $accAssembly) { return $false }
    Add-Type -ReferencedAssemblies $accAssembly.Location -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;
using Accessibility;

public static class DeskCardspecMenu {
    [DllImport("oleacc.dll")]
    static extern int AccessibleObjectFromWindow(IntPtr hwnd, uint id, ref Guid iid,
        [MarshalAs(UnmanagedType.IUnknown)] out object obj);

    const uint ObjIdClient = 4294967292u;

    static T RunSta<T>(Func<T> body, int timeoutMs) {
        T result = default(T);
        Thread thread = new Thread(delegate() { try { result = body(); } catch { } });
        thread.IsBackground = true;
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        thread.Join(timeoutMs);
        return result;
    }

    public static string[] Names(IntPtr menu) {
        return RunSta<string[]>(delegate() {
            Guid iid = new Guid("618736E0-3C3D-11CF-810C-00AA00389B71");
            object obj = null;
            if (AccessibleObjectFromWindow(menu, ObjIdClient, ref iid, out obj) != 0 || obj == null) {
                return new string[0];
            }
            IAccessible acc = (IAccessible)obj;
            int count = acc.accChildCount;
            List<string> names = new List<string>();
            for (int child = 1; child <= count; ++child) {
                string name = "";
                try { name = acc.get_accName(child); } catch { continue; }
                if (name == null) continue;
                names.Add(name);
            }
            return names.ToArray();
        }, 4000);
    }

    public static bool Invoke(IntPtr menu, string pattern) {
        bool[] found = new bool[1];
        ManualResetEvent ready = new ManualResetEvent(false);
        Thread thread = new Thread(delegate() {
            try {
                Guid iid = new Guid("618736E0-3C3D-11CF-810C-00AA00389B71");
                object obj = null;
                if (AccessibleObjectFromWindow(menu, ObjIdClient, ref iid, out obj) != 0 || obj == null) return;
                IAccessible acc = (IAccessible)obj;
                int count = acc.accChildCount;
                for (int child = 1; child <= count; ++child) {
                    string name = "";
                    try { name = acc.get_accName(child); } catch { continue; }
                    if (name == null || !Glob(pattern, name)) continue;
                    found[0] = true;
                    ready.Set();
                    try { acc.accDoDefaultAction(child); } catch { }
                    return;
                }
            } catch {
            } finally {
                ready.Set();
            }
        });
        thread.IsBackground = true;
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        ready.WaitOne(4000);
        return found[0];
    }

    static bool Glob(string pattern, string text) {
        if (pattern.EndsWith("*")) {
            return text.StartsWith(pattern.Substring(0, pattern.Length - 1), StringComparison.OrdinalIgnoreCase);
        }
        return string.Equals(pattern, text, StringComparison.OrdinalIgnoreCase);
    }
}
'@ -ErrorAction Stop
    return ('DeskCardspecMenu' -as [type]) -ne $null
}

function Initialize-CsWin32 {
    if ('DeskCardspecWin32' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class DeskCardspecWin32 {
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr dialog, int id);

    [DllImport("user32.dll", SetLastError = true)]
    static extern IntPtr SendMessageTimeout(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp,
        uint flags, uint timeout, out IntPtr result);

    // SMTO_ABORTIFHUNG|SMTO_BLOCK: an unresponsive board (open modal menu, hung
    // worker) must make the caller fail its check instead of hanging the harness.
    public static bool SendTimeout(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp, uint timeoutMs,
                                   out long result) {
        IntPtr res;
        IntPtr ok = SendMessageTimeout(hwnd, msg, wp, lp, 0x0002u | 0x0001u, timeoutMs, out res);
        result = res.ToInt64();
        return ok != IntPtr.Zero;
    }
}
'@ -ErrorAction Stop
}

function Send-CsMessageTimeout {
    # Every cross-process SendMessage in this script goes through this guarded
    # call (own-canvas export and dialog buttons).
    param(
        [IntPtr]$Hwnd,
        [uint32]$Message,
        [IntPtr]$WParam = [IntPtr]::Zero,
        [IntPtr]$LParam = [IntPtr]::Zero,
        [int]$TimeoutMs = 5000
    )
    Initialize-CsWin32
    $result = [long]0
    $ok = [DeskCardspecWin32]::SendTimeout($Hwnd, $Message, $WParam, $LParam, [uint32]$TimeoutMs, [ref]$result)
    return [pscustomobject]@{ Ok = $ok; Result = $result }
}

function Initialize-CsImage {
    # Pixel probes specific to this acceptance: exact-colour counts (theme fills),
    # a card bottom-border row finder, and a column diff for the bar comparison.
    # desk-testlib already provides the luminance ink helpers.
    if ('DeskCardspecImage' -as [type]) { return }
    Add-Type -ReferencedAssemblies 'System.Drawing' -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class DeskCardspecImage {
    static byte[] Lock(Bitmap bmp, out int stride, out int width, out int height) {
        Rectangle rect = new Rectangle(0, 0, bmp.Width, bmp.Height);
        BitmapData data = bmp.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        stride = data.Stride;
        width = bmp.Width;
        height = bmp.Height;
        byte[] bytes = new byte[Math.Abs(stride) * height];
        Marshal.Copy(data.Scan0, bytes, 0, bytes.Length);
        bmp.UnlockBits(data);
        return bytes;
    }

    static bool Near(byte[] b, int i, int r, int g, int bl, int tol) {
        return Math.Abs(b[i] - bl) <= tol && Math.Abs(b[i + 1] - g) <= tol && Math.Abs(b[i + 2] - r) <= tol;
    }

    // Pixels within tol of an exact theme colour (the DIB is BGRA; RGB passed in).
    public static int CountNear(Bitmap bmp, int x0, int y0, int w, int h, int r, int g, int bl, int tol) {
        if (bmp == null) return 0;
        int stride, width, height;
        byte[] bytes = Lock(bmp, out stride, out width, out height);
        x0 = Math.Max(0, x0); y0 = Math.Max(0, y0);
        w = Math.Min(w, width - x0); h = Math.Min(h, height - y0);
        int count = 0;
        for (int y = y0; y < y0 + h; ++y) {
            int row = y * stride;
            for (int x = x0; x < x0 + w; ++x) {
                if (Near(bytes, row + x * 4, r, g, bl, tol)) ++count;
            }
        }
        return count;
    }

    // First row at or after yStart whose exact theme-coloured run inside
    // [x0, x1) reaches minCount columns. Used for the card bottom border.
    public static int FindRowWithCount(Bitmap bmp, int x0, int x1, int yStart, int yEnd,
                                       int r, int g, int bl, int tol, int minCount) {
        if (bmp == null) return -1;
        int stride, width, height;
        byte[] bytes = Lock(bmp, out stride, out width, out height);
        yStart = Math.Max(0, yStart);
        yEnd = Math.Min(yEnd, height);
        x0 = Math.Max(0, x0); x1 = Math.Min(x1, width);
        for (int y = yStart; y < yEnd; ++y) {
            int row = y * stride;
            int count = 0;
            for (int x = x0; x < x1; ++x) {
                if (Near(bytes, row + x * 4, r, g, bl, tol)) ++count;
            }
            if (count >= minCount) return y;
        }
        return -1;
    }

    // Columns in [x0, x1) that differ at any row of [y0, y1) by >= threshold.
    public static int ColumnDiff(Bitmap a, Bitmap b, int x0, int x1, int y0, int y1, int threshold) {
        if (a == null || b == null) return -1;
        int strideA, wA, hA, strideB, wB, hB;
        byte[] ba = Lock(a, out strideA, out wA, out hA);
        byte[] bb = Lock(b, out strideB, out wB, out hB);
        x0 = Math.Max(0, x0); y0 = Math.Max(0, y0);
        x1 = Math.Min(x1, Math.Min(wA, wB));
        y1 = Math.Min(y1, Math.Min(hA, hB));
        int columns = 0;
        for (int x = x0; x < x1; ++x) {
            bool differs = false;
            for (int y = y0; y < y1; ++y) {
                int ia = y * strideA + x * 4;
                int ib = y * strideB + x * 4;
                int d = Math.Abs(ba[ia] - bb[ib]) + Math.Abs(ba[ia + 1] - bb[ib + 1]) +
                        Math.Abs(ba[ia + 2] - bb[ib + 2]);
                if (d >= threshold) { differs = true; break; }
            }
            if (differs) ++columns;
        }
        return columns;
    }
}
'@ -ErrorAction Stop
}

# ---------------------------------------------------------------------------
# path guard + evidence writers
# ---------------------------------------------------------------------------

function Assert-CsInsideRun {
    param([string]$Path)
    if ($null -eq $script:Run) { throw ('run layout is not initialised: refusing to write ' + $Path) }
    $full = [System.IO.Path]::GetFullPath($Path)
    # Trailing separator: "...\run" must not accept "...\run-evil" as inside.
    $root = [System.IO.Path]::GetFullPath($script:Run.Dir).TrimEnd([char]92, [char]47) + [System.IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw ('refusing to write outside the run directory: ' + $full)
    }
}

function Write-CsFile {
    param([string]$Path, [string]$Text)
    Assert-CsInsideRun -Path $Path
    $directory = Split-Path -Parent $Path
    if ($directory -and -not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    [System.IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
}

function Write-CsJson {
    param([string]$Path, [object]$Value)
    Write-CsFile -Path $Path -Text ($Value | ConvertTo-Json -Depth 12)
}

function Read-CsText {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }
    try { return [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8) } catch { return '' }
}

function Write-CsIni {
    # wg::Store files are UTF-16LE with exactly ONE BOM. desk-testlib's
    # Write-DeskIniFile prepends a BOM character AND writes with Encoding.Unicode
    # (which emits its own BOM), so the first key would silently become
    # "\uFEFFhttp.profile". Write the bytes here instead.
    param([string]$Path, [System.Collections.IDictionary]$Values)
    Assert-CsInsideRun -Path $Path
    $sb = New-Object System.Text.StringBuilder
    foreach ($key in $Values.Keys) { [void]$sb.Append("$key=$($Values[$key])" + [char]10) }
    $directory = Split-Path -Parent $Path
    if ($directory -and -not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    [System.IO.File]::WriteAllText($Path, $sb.ToString(), [System.Text.Encoding]::Unicode)
}

function Write-CsSpecFile {
    # cardspec.json is plain UTF-8 without BOM (the loader accepts an optional
    # BOM but never requires one).
    param([string]$ConfigDir, [object]$Spec)
    Write-CsFile -Path (Join-Path $ConfigDir 'cardspec.json') -Text (($Spec | ConvertTo-Json -Depth 8 -Compress))
}

function New-CsSpecRows {
    param([string]$Title, [object[]]$Rows)
    return [ordered]@{
        version = 1
        title   = $Title
        source  = [ordered]@{ type = 'inline' }
        layout  = 'rows'
        rows    = [object[]]$Rows
    }
}

function New-CsSpecBar {
    param([string]$Title, [double]$Value, [double]$Max)
    return [ordered]@{
        version = 1
        title   = $Title
        source  = [ordered]@{ type = 'inline' }
        layout  = 'bar'
        rows    = [object[]]@([ordered]@{ label = '学习'; value = $Value; max = $Max })
    }
}

function New-CsSpecHttpRows {
    param([string]$Title, [string]$Profile)
    return [ordered]@{
        version = 2
        title   = $Title
        source  = [ordered]@{ type = 'http'; profile = $Profile }
        layout  = 'rows'
        rows    = [object[]]@(
            [ordered]@{ label = '名称'; text_path = '/items/0/name' },
            [ordered]@{ label = '响应'; text_path = '/items/0/text' }
        )
    }
}

# ---------------------------------------------------------------------------
# run layout
# ---------------------------------------------------------------------------

function Get-CsMainWorktreeRoot {
    # Linked worktrees live at <main>\_worktrees\<name>; there .git is a
    # directory, while a linked worktree carries a .git file. Read-only.
    param([string]$RepoRoot)
    if (-not $RepoRoot) { return '' }
    $parent = Split-Path -Parent $RepoRoot
    if (-not $parent) { return '' }
    if ((Split-Path -Leaf $parent) -ne '_worktrees') { return '' }
    $main = Split-Path -Parent $parent
    if (-not $main) { return '' }
    if (-not (Test-Path -LiteralPath (Join-Path $main '.git') -PathType Container)) { return '' }
    return [System.IO.Path]::GetFullPath($main)
}

function New-CsRunLayout {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $dir = Join-Path $script:QaRoot $stamp
    $counter = 1
    while (Test-Path -LiteralPath $dir) {
        $dir = Join-Path $script:QaRoot ('{0}-{1}' -f $stamp, $counter)
        $counter++
    }
    $layout = [pscustomobject]@{
        Dir          = $dir
        AppDir       = Join-Path $dir 'app'
        ConfigInline = Join-Path $dir 'config-inline'
        ConfigHttp   = Join-Path $dir 'config-http'
        ConfigInvalid = Join-Path $dir 'does-not-exist\nested'
        DisplayFile  = Join-Path $dir 'fixture\display.txt'
        AppData      = Join-Path $dir 'profile\appdata'
        LocalAppData = Join-Path $dir 'profile\localappdata'
        Temp         = Join-Path $dir 'profile\temp'
        UserProfile  = Join-Path $dir 'profile\user'
        EvidenceDir  = Join-Path $dir 'evidence'
        SuffixInline = ('csq{0}' -f ((Get-Random -Minimum 10000 -Maximum 99999)))
        SuffixHttp   = ('csq{0}' -f ((Get-Random -Minimum 10000 -Maximum 99999)))
        SuffixNegA   = ('csq{0}' -f ((Get-Random -Minimum 10000 -Maximum 99999)))
        SuffixNegB   = ('csq{0}' -f ((Get-Random -Minimum 10000 -Maximum 99999)))
        Exe          = ''
        ClassInline  = ''
        ClassHttp    = ''
        SourceExe    = ''
        RuntimeDlls  = [string[]]@()
    }
    foreach ($path in @($layout.Dir, $layout.AppDir, $layout.ConfigInline, $layout.ConfigHttp,
                        $layout.AppData, $layout.LocalAppData, $layout.Temp, $layout.UserProfile,
                        $layout.EvidenceDir)) {
        New-Item -ItemType Directory -Path $path -Force | Out-Null
    }
    $layout.Exe = Join-Path $layout.AppDir 'desk.exe'
    $layout.ClassInline = 'wg.desk-' + $layout.SuffixInline
    $layout.ClassHttp = 'wg.desk-' + $layout.SuffixHttp
    # Fixed scripted display so board geometry never depends on the real desktop.
    Write-DeskDisplayFile -Path $layout.DisplayFile -Work '0,0,2560,1392' -Dpi 96
    return $layout
}

function Copy-CsBoardImage {
    # One self-contained copy. Same-build MinGW runtime DLLs that sit beside the
    # source exe are copied too; PATH is never extended and nothing is installed.
    param([object]$Run, [string]$SourceExe)
    Copy-Item -LiteralPath $SourceExe -Destination $Run.Exe -Force
    $sourceDir = Split-Path -Parent $SourceExe
    $copied = New-Object System.Collections.Generic.List[string]
    foreach ($dll in @(Get-ChildItem -LiteralPath $sourceDir -Filter '*.dll' -File -ErrorAction SilentlyContinue)) {
        Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $Run.AppDir $dll.Name) -Force
        $copied.Add($dll.Name)
    }
    $Run.RuntimeDlls = [string[]]$copied.ToArray()
}

# ---------------------------------------------------------------------------
# static / self checks
# ---------------------------------------------------------------------------

function Find-CsByteSequence {
    param([byte[]]$Haystack, [byte[]]$Needle)
    if ($null -eq $Haystack -or $null -eq $Needle -or $Needle.Length -eq 0) { return $false }
    $limit = $Haystack.Length - $Needle.Length
    for ($i = 0; $i -le $limit; $i++) {
        $match = $true
        for ($j = 0; $j -lt $Needle.Length; $j++) {
            if ($Haystack[$i + $j] -ne $Needle[$j]) { $match = $false; break }
        }
        if ($match) { return $true }
    }
    return $false
}

function Test-CsImageLiteral {
    # The fixture body is a plain string literal in the image; search the
    # UTF-16LE and UTF-8 encodings.
    param([string]$Path, [string]$Needle)
    try { $bytes = [System.IO.File]::ReadAllBytes($Path) } catch { return $false }
    if (Find-CsByteSequence -Haystack $bytes -Needle ([System.Text.Encoding]::Unicode.GetBytes($Needle))) { return $true }
    if (Find-CsByteSequence -Haystack $bytes -Needle ([System.Text.Encoding]::UTF8.GetBytes($Needle))) { return $true }
    return $false
}

function Invoke-CsStaticSuite {
    param([object]$Result, [string]$ScriptPath, [string]$ExePath)
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($ScriptPath, [ref]$tokens, [ref]$errors)
    $parseErrors = @($errors)
    $detail = 'none'
    if ($parseErrors.Count -gt 0) {
        $detail = (@($parseErrors | ForEach-Object {
            'line {0}: {1}' -f $_.Extent.StartLineNumber, $_.Message
        }) -join '; ')
    }
    [void](Add-CsCheck -Result $Result -Name 'static: script AST parses without errors' `
        -Pass ($parseErrors.Count -eq 0) -Detail $detail)
    $functions = @($ast.FindAll({ param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true))
    $entryPoints = @($functions | Where-Object {
        $_.Name -eq 'Invoke-CsInlineSuite' -or $_.Name -eq 'Invoke-CsHttpSuite' -or
        $_.Name -eq 'Invoke-CsNegativeSuite'
    })
    $Result.Evidence.Add(('AST: {0} tokens, {1} functions, param block={2}' -f
        @($tokens).Count, $functions.Count, @($ast.ParamBlock).Count))
    [void](Add-CsCheck -Result $Result -Name 'static: script defines the expected entry points' `
        -Pass (($null -ne $ast.ParamBlock) -and $entryPoints.Count -eq 3) `
        -Detail ('entry points=' + $entryPoints.Count + ' functions=' + $functions.Count))

    # The banned tokens are assembled from fragments so this file never contains
    # the very strings it forbids.
    $text = Read-CsText -Path $ScriptPath
    $banned = @(
        ('CopyFrom' + 'Screen'),
        ('Bit' + 'Blt'),
        ('Invoke-Web' + 'Request'),
        ('Invoke-Rest' + 'Method'),
        ('Start-Bits' + 'Transfer'),
        ('Test-Net' + 'Connection'),
        ('Resolve-' + 'DnsName'),
        ('git ' + 'push'),
        ('git ' + 'commit'),
        ('git ' + 'merge'),
        ('wing' + 'et '),
        ('msie' + 'xec '),
        ('Uninstall-' + 'Package'),
        ('Install-' + 'Package')
    )
    $hits = New-Object System.Collections.Generic.List[string]
    foreach ($needle in $banned) {
        if ($text.IndexOf($needle, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) { $hits.Add($needle) }
    }
    [void](Add-CsCheck -Result $Result -Name 'static: script contains no screen capture, network or install command' `
        -Pass ($hits.Count -eq 0) -Detail ('hits=' + $(if ($hits.Count -gt 0) { $hits.ToArray() -join ',' } else { 'none' })))
    [void](Add-CsCheck -Result $Result -Name 'static: every child process goes through the guarded launcher' `
        -Pass ($text.IndexOf(('Start-' + 'Process'), [System.StringComparison]::Ordinal) -lt 0) `
        -Detail ('launcher call sites=' + @([regex]::Matches($text, 'Start-Cs(Board|NegativeInstance)')).Count))
    [void](Add-CsCheck -Result $Result -Name 'static: evidence writes are guarded to the run directory' `
        -Pass ($text.IndexOf('Assert-CsInsideRun', [System.StringComparison]::Ordinal) -ge 0) -Detail 'path guard present')

    # PowerShell 5.1 decodes a script file with the ANSI code page unless the
    # file starts with a UTF-8 BOM; without it every Chinese expectation in this
    # file would silently become mojibake. Check the bytes, not the text.
    $bomOk = $false
    try {
        $head = [System.IO.File]::ReadAllBytes($ScriptPath)
        $bomOk = ($head.Length -ge 3 -and $head[0] -eq 0xEF -and $head[1] -eq 0xBB -and $head[2] -eq 0xBF)
    } catch { }
    [void](Add-CsCheck -Result $Result -Name 'static: script is UTF-8 with BOM (PowerShell 5.1 safe)' -Pass $bomOk `
        -Detail ('bom=' + $bomOk))

    # The C# helper types cannot be validated by the PowerShell AST; compile them
    # here so a syntax slip is a static failure instead of a mid-run crash.
    $compileErrors = New-Object System.Collections.Generic.List[string]
    foreach ($initializer in @('Initialize-CsLaunch', 'Initialize-CsMenu', 'Initialize-CsWin32', 'Initialize-CsImage')) {
        try {
            & $initializer | Out-Null
        } catch {
            $compileErrors.Add($initializer + ': ' + $_.Exception.Message)
        }
    }
    [void](Add-CsCheck -Result $Result -Name 'static: native helper types compile' `
        -Pass ($compileErrors.Count -eq 0) `
        -Detail $(if ($compileErrors.Count -gt 0) { $compileErrors.ToArray() -join ' | ' } else { 'launcher, menu, win32, image' }))

    if ($ExePath -and (Test-Path -LiteralPath $ExePath -PathType Leaf)) {
        [void](Add-CsCheck -Result $Result -Name 'static: board image has the DESK_TEST_CARDSPEC_ONLY hook' `
            -Pass (Test-CsImageLiteral -Path $ExePath -Needle 'DESK_TEST_CARDSPEC_ONLY') -Detail $ExePath)
        [void](Add-CsCheck -Result $Result -Name 'static: board image has the fixed synthetic HTTP body' `
            -Pass (Test-CsImageLiteral -Path $ExePath -Needle $script:FixtureBody) -Detail 'fixed fixture literal present')
        [void](Add-CsCheck -Result $Result -Name 'static: board image has the own-canvas export path' `
            -Pass (Test-CsImageLiteral -Path $ExePath -Needle 'assistant-test-frame-') -Detail 'WM_APP+11 hook present')
        [void](Add-CsCheck -Result $Result -Name 'static: board image has the fetch counter file name' `
            -Pass (Test-CsImageLiteral -Path $ExePath -Needle 'cardspec-test-fetch-count.txt') -Detail 'counter wiring present')
    } else {
        [void](Add-CsCheck -Result $Result -Name 'static: board image is available for hook checks' `
            -Pass $false -Status 'blocked' -Detail ('missing exe: ' + $ExePath))
    }
}

# ---------------------------------------------------------------------------
# process / window helpers
# ---------------------------------------------------------------------------

function Get-CsCreationTicks {
    param($Value)
    if ($null -eq $Value) { return $null }
    if ($Value -is [datetime]) { return $Value.ToUniversalTime().Ticks }
    try { return ([datetime]$Value).ToUniversalTime().Ticks } catch { return $null }
}

function Get-CsProcessIdentity {
    param([int]$ProcessId)
    $process = Get-CimInstance Win32_Process -Filter "ProcessId=$ProcessId" -ErrorAction SilentlyContinue
    if (-not $process) { return $null }
    $path = ''
    try { $path = [string]$process.ExecutablePath } catch { }
    return [pscustomobject]@{
        ProcessId = [int]$process.ProcessId
        Name = [string]$process.Name
        CreationTicks = (Get-CsCreationTicks $process.CreationDate)
        ExecutablePath = $path
    }
}

function Test-CsSameProcess {
    # A reused PID, an unreadable identity or a different image is never "ours".
    param([object]$Recorded)
    if ($null -eq $Recorded) { return 'gone' }
    $current = Get-CsProcessIdentity -ProcessId ([int]$Recorded.ProcessId)
    if (-not $current) { return 'gone' }
    if ($null -eq $Recorded.CreationTicks -or $null -eq $current.CreationTicks) { return 'unverified' }
    if ([long]$Recorded.CreationTicks -ne [long]$current.CreationTicks) { return 'reused' }
    if (-not $Recorded.ExecutablePath -or -not $current.ExecutablePath) { return 'unverified' }
    $old = [System.IO.Path]::GetFullPath([string]$Recorded.ExecutablePath)
    $new = [System.IO.Path]::GetFullPath([string]$current.ExecutablePath)
    if (-not [string]::Equals($old, $new, [System.StringComparison]::OrdinalIgnoreCase)) { return 'reused' }
    return 'same'
}

function Stop-CsInstance {
    # Only the recorded PID, only after re-proving identity, only when the image
    # lives under this run's app directory.
    param([object]$Recorded, [object]$Run)
    if ($null -eq $Recorded) { return 'gone' }
    $state = Test-CsSameProcess -Recorded $Recorded
    if ($state -ne 'same') { return $state }
    $full = [System.IO.Path]::GetFullPath([string]$Recorded.ExecutablePath)
    # Trailing separator: "...\app" must not accept "...\app-evil\desk.exe".
    $root = [System.IO.Path]::GetFullPath($Run.AppDir).TrimEnd([char]92, [char]47) + [System.IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) { return 'outside-root' }
    Stop-Process -Id ([int]$Recorded.ProcessId) -Force -ErrorAction SilentlyContinue
    for ($i = 0; $i -lt 40; $i++) {
        if (-not (Test-DeskTestAlive -ProcessId ([int]$Recorded.ProcessId))) { break }
        Start-Sleep -Milliseconds 100
    }
    if (Test-DeskTestAlive -ProcessId ([int]$Recorded.ProcessId)) { return 'stuck' }
    return 'stopped'
}

function Get-CsChildEnvironment {
    # Every isolation variable exists only in the child environment. APPDATA,
    # LOCALAPPDATA, TEMP and USERPROFILE point into the run directory so even a
    # misbehaving child cannot write into the real user profile.
    param([object]$Run, [string]$ConfigDir, [string]$Suffix)
    $environment = [ordered]@{
        DESK_TEST_CARDSPEC_ONLY = '1'
        DESK_TEST_DISPLAY_FILE  = $Run.DisplayFile
        APPDATA                 = $Run.AppData
        LOCALAPPDATA            = $Run.LocalAppData
        TEMP                    = $Run.Temp
        TMP                     = $Run.Temp
        USERPROFILE             = $Run.UserProfile
        HOME                    = $Run.UserProfile
    }
    if ($Suffix) { $environment['DESK_TEST_INSTANCE'] = $Suffix }
    if ($ConfigDir) { $environment['DESK_TEST_CONFIG_DIR'] = $ConfigDir }
    return $environment
}

function Start-CsObserved {
    param([object]$Run, [string]$Tag, [System.Collections.IDictionary]$Environment)
    Initialize-CsLaunch
    $names = [string[]]@($Environment.Keys)
    $values = [string[]]::new($names.Count)
    for ($i = 0; $i -lt $names.Count; $i++) { $values[$i] = [string]$Environment[$names[$i]] }
    $stdout = Join-Path $Run.EvidenceDir ($Tag + '.stdout.txt')
    $stderr = Join-Path $Run.EvidenceDir ($Tag + '.stderr.txt')
    $processId = [DeskCardspecLaunch]::StartObserved($Run.Exe, $Run.AppDir, $names, $values, $stdout, $stderr)
    return [pscustomobject]@{
        Tag = $Tag
        ProcessId = $processId
        Record = (Get-CsProcessIdentity -ProcessId $processId)
        Hwnd = [IntPtr]::Zero
        Suffix = ''
        ClassName = ''
        ConfigDir = ''
        Closed = $false
    }
}

function Start-CsBoard {
    param([object]$Run, [string]$ConfigDir, [string]$Suffix, [string]$Tag)
    $entry = Start-CsObserved -Run $Run -Tag $Tag -Environment (Get-CsChildEnvironment -Run $Run -ConfigDir $ConfigDir -Suffix $Suffix)
    $entry.Suffix = $Suffix
    $entry.ClassName = 'wg.desk-' + $Suffix
    $entry.ConfigDir = $ConfigDir
    $script:Boards.Add($entry)
    return $entry
}

function Start-CsNegativeInstance {
    param([object]$Run, [string]$Tag, [System.Collections.IDictionary]$Environment)
    return (Start-CsObserved -Run $Run -Tag $Tag -Environment $Environment)
}

function Wait-CsBoardWindow {
    param([object]$Board, [int]$TimeoutSeconds = 25)
    $windows = @(Wait-DeskWindow -ProcessId $Board.ProcessId -ClassName $Board.ClassName -TimeoutSeconds $TimeoutSeconds)
    if ($windows.Count -eq 0) { return $null }
    $Board.Hwnd = $windows[0].Hwnd
    return $windows[0]
}

function Save-CsWindowDiagnostic {
    # Non-assertive evidence for a board that never produced the expected
    # window: every window the PID owns (metadata only) plus the real exit code.
    param([object]$Run, [object]$Board, [string]$Path)
    $windows = @()
    try { $windows = @(Get-DeskWindowList -ProcessId $Board.ProcessId) } catch { }
    $exited = $null
    $exitCode = $null
    try {
        $trackedExited = $false
        $trackedCode = 0
        if ([DeskCardspecLaunch]::TryGetExit($Board.ProcessId, [ref]$trackedExited, [ref]$trackedCode)) {
            $exited = $trackedExited
            $exitCode = $trackedCode
        }
    } catch { }
    $record = [pscustomobject]@{
        tag           = [string]$Board.Tag
        processId     = [int]$Board.ProcessId
        expectedClass = [string]$Board.ClassName
        seenClasses   = [string[]]@(@($windows | ForEach-Object { [string]$_.Class } | Select-Object -Unique))
        windowCount   = $windows.Count
        processAlive  = ($null -ne (Get-Process -Id $Board.ProcessId -ErrorAction SilentlyContinue))
        exitKnown     = ($null -ne $exited)
        exited        = $exited
        exitCode      = $exitCode
        imagePath     = (Get-DeskTestPath -ProcessId $Board.ProcessId)
    }
    if ($Path) { Write-CsJson -Path $Path -Value $record }
    return $record
}

function Close-CsBoard {
    param([object]$Run, [object]$Board, [object]$Result, [string]$Prefix)
    if ($null -eq $Board -or $Board.ProcessId -le 0) { return }
    if ($Board.Closed) { return }
    $Board.Closed = $true
    if ($Board.Hwnd -ne [IntPtr]::Zero) {
        [void][DeskTestNative]::PostMessage($Board.Hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    }
    $graceful = Wait-DeskTestExit -ProcessId $Board.ProcessId -TimeoutMs 12000
    if ($graceful) {
        [void](Add-CsCheck -Result $Result -Name ($Prefix + ' board closes within 12s') -Pass $true -Detail 'graceful WM_CLOSE')
    } else {
        $state = Stop-CsInstance -Recorded $Board.Record -Run $Run
        [void](Add-CsCheck -Result $Result -Name ($Prefix + ' board closes within 12s') `
            -Pass ($state -eq 'stopped' -or $state -eq 'gone') -Detail ('forced stop: ' + $state))
    }
    $exited = $false
    $exitCode = 0
    if ([DeskCardspecLaunch]::TryGetExit($Board.ProcessId, [ref]$exited, [ref]$exitCode)) {
        $Result.Evidence.Add(('{0} exit: exited={1} code={2}' -f $Prefix, $exited, $exitCode))
        [DeskCardspecLaunch]::ForgetTracked($Board.ProcessId)
    }
}

# ---------------------------------------------------------------------------
# menu driving (real Win32 popups: MSAA first, keyboard fallback)
# ---------------------------------------------------------------------------

function Wait-CsMenuReady {
    if ('DeskCardspecMenu' -as [type]) { return $true }
    try { return (Initialize-CsMenu) } catch { return $false }
}

function Wait-CsPopup {
    param([int]$ProcessId, [int]$TimeoutMs = 3000, [IntPtr[]]$Exclude = @())
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline) {
        $list = @(Get-DeskWindowList -ProcessId $ProcessId | Where-Object { $_.Class -eq '#32768' -and $_.Visible })
        foreach ($window in $list) {
            $skip = $false
            foreach ($excluded in $Exclude) {
                if ($excluded -ne [IntPtr]::Zero -and $window.Hwnd -eq $excluded) { $skip = $true }
            }
            if (-not $skip) { return $window }
        }
        Start-Sleep -Milliseconds 100
    }
    return $null
}

function Send-CsMenuKeys {
    param([IntPtr]$Hwnd, [int[]]$Keys, [int]$GapMs = 140)
    foreach ($key in $Keys) {
        [void][DeskTestNative]::PostMessage($Hwnd, 0x0100, [IntPtr]$key, [IntPtr]1)
        Start-Sleep -Milliseconds $GapMs
    }
}

function Get-CsMenuNames {
    param([IntPtr]$Menu)
    if (-not (Wait-CsMenuReady)) { return $null }
    try { return [string[]]@([DeskCardspecMenu]::Names($Menu)) } catch { return $null }
}

function Invoke-CsMenuItem {
    param([IntPtr]$Menu, [string]$Pattern, [object]$Result, [string]$Label)
    if (-not (Wait-CsMenuReady)) {
        if ($Result) { $Result.Evidence.Add($Label + ': MSAA unavailable') }
        return $false
    }
    try {
        if ([DeskCardspecMenu]::Invoke($Menu, $Pattern)) { return $true }
    } catch {
        if ($Result) { $Result.Evidence.Add(('{0}: MSAA invoke threw: {1}' -f $Label, $_.Exception.Message)) }
    }
    return $false
}

function Close-CsPopup {
    param([object]$Popup)
    if ($null -ne $Popup) {
        [void][DeskTestNative]::PostKey($Popup.Hwnd, 0x1B)
        Start-Sleep -Milliseconds 200
    }
}

function Open-CsBoardMenu {
    # Right click on the board title strip (outside the card interior) opens the
    # real board menu. board.cpp uses GetCursorPos for the popup position, so no
    # real mouse movement is needed and nothing is injected system-wide.
    param([object]$Board)
    [void][DeskTestNative]::PostMessage($Board.Hwnd, 0x0204, [IntPtr]1, [DeskTestNative]::MakeLParam(54, 10))
    [void][DeskTestNative]::PostMessage($Board.Hwnd, 0x0205, [IntPtr]::Zero, [DeskTestNative]::MakeLParam(54, 10))
    return (Wait-CsPopup -ProcessId $Board.ProcessId -TimeoutMs 4000)
}

function Open-CsCardMenu {
    param([object]$Board, [object]$Card)
    $x = [int]$Card.X + 40
    $y = [int]$Card.Y + 12
    [void][DeskTestNative]::PostMessage($Board.Hwnd, 0x0204, [IntPtr]1, [DeskTestNative]::MakeLParam($x, $y))
    [void][DeskTestNative]::PostMessage($Board.Hwnd, 0x0205, [IntPtr]::Zero, [DeskTestNative]::MakeLParam($x, $y))
    return (Wait-CsPopup -ProcessId $Board.ProcessId -TimeoutMs 4000)
}

function Add-CsCardspecModule {
    # The acceptance path is the real Add module menu, never a preset visible=1.
    param([object]$Board, [object]$Result, [string]$Prefix)
    $menu = Open-CsBoardMenu -Board $Board
    if (-not $menu) {
        [void](Add-CsCheck -Result $Result -Name ($Prefix + ' board menu opens') -Pass $false `
            -Detail 'no #32768 popup owned by the board')
        return $false
    }
    if (-not (Wait-CsMenuReady)) {
        Close-CsPopup -Popup $menu
        [void](Add-CsCheck -Result $Result -Name ($Prefix + ' MSAA menu automation is available') `
            -Pass $false -Status 'blocked' -Detail 'Accessibility assembly or STA MSAA helper unavailable')
        return $false
    }
    $boardNames = Get-CsMenuNames -Menu $menu.Hwnd
    $Result.Evidence.Add(('{0} board menu items: {1}' -f $Prefix, ($boardNames -join ' | ')))
    [void](Add-CsCheck -Result $Result -Name ($Prefix + ' board menu offers Add module') `
        -Pass (@($boardNames | Where-Object { $_ -like 'Add module*' }).Count -eq 1) -Detail ($boardNames -join ' | '))

    [void](Invoke-CsMenuItem -Menu $menu.Hwnd -Pattern 'Add module*' -Result $Result -Label ($Prefix + ' Add module'))
    $submenu = Wait-CsPopup -ProcessId $Board.ProcessId -TimeoutMs 3000 -Exclude @($menu.Hwnd)
    if (-not $submenu) {
        # Keyboard fallback: HOME selects the first item (the Add module popup),
        # RIGHT opens it.
        Send-CsMenuKeys -Hwnd $menu.Hwnd -Keys @(0x24, 0x27)
        $submenu = Wait-CsPopup -ProcessId $Board.ProcessId -TimeoutMs 3000 -Exclude @($menu.Hwnd)
    }
    if (-not $submenu) {
        Close-CsPopup -Popup $menu
        [void](Add-CsCheck -Result $Result -Name ($Prefix + ' Add module submenu opens') -Pass $false `
            -Detail 'no submenu popup appeared')
        return $false
    }
    $names = Get-CsMenuNames -Menu $submenu.Hwnd
    $Result.Evidence.Add(('{0} Add module items: {1}' -f $Prefix, ($names -join ' | ')))
    $otherModules = @($names | Where-Object { $_ -match '^(?i)(Account|Servers|PowerShell|Assistant|Memory)' })
    [void](Add-CsCheck -Result $Result -Name ($Prefix + ' Add module lists only CARDSPEC') `
        -Pass (@($names | Where-Object { $_ -like 'CARDSPEC*' }).Count -eq 1 -and $otherModules.Count -eq 0) `
        -Detail ('items=' + ($names -join ' | ')))

    $invoked = Invoke-CsMenuItem -Menu $submenu.Hwnd -Pattern 'CARDSPEC*' -Result $Result -Label ($Prefix + ' CARDSPEC')
    if (-not $invoked) {
        # Keyboard fallback inside the submenu: HOME selects the first item.
        Send-CsMenuKeys -Hwnd $submenu.Hwnd -Keys @(0x24, 0x0D)
    }
    Start-Sleep -Milliseconds 800
    return $true
}

function Invoke-CsCardMenuItem {
    param([object]$Board, [object]$Card, [string]$Pattern, [object]$Result, [string]$Prefix)
    $menu = Open-CsCardMenu -Board $Board -Card $Card
    if (-not $menu) {
        [void](Add-CsCheck -Result $Result -Name ($Prefix + ' card menu opens') -Pass $false -Detail 'no #32768 popup')
        return $null
    }
    $names = Get-CsMenuNames -Menu $menu.Hwnd
    if (-not (Wait-CsMenuReady)) {
        # Without MSAA only the first item can be reached reliably (HOME +
        # ENTER), which is exactly the module's own "Reload cardspec.json"
        # entry. Anything else is reported as blocked, never silently guessed.
        if ($Pattern -like 'Reload cardspec.json*') {
            Send-CsMenuKeys -Hwnd $menu.Hwnd -Keys @(0x24, 0x0D)
            Start-Sleep -Milliseconds 400
            return $names
        }
        Close-CsPopup -Popup $menu
        [void](Add-CsCheck -Result $Result -Name ($Prefix + ' MSAA menu automation is available') `
            -Pass $false -Status 'blocked' -Detail 'only the first card-menu item is reachable without MSAA')
        return $null
    }
    if (-not (Invoke-CsMenuItem -Menu $menu.Hwnd -Pattern $Pattern -Result $Result -Label $Prefix)) {
        # Keyboard fallback: HOME selects the first item ("Reload cardspec.json").
        Send-CsMenuKeys -Hwnd $menu.Hwnd -Keys @(0x24, 0x0D)
        Start-Sleep -Milliseconds 400
    }
    return $names
}

function Start-CsApproveCommand {
    # Fires the approve command. It opens the real MessageBox on the board UI
    # thread, so the menu is driven from an STA background thread and the dialog
    # is located here.
    param([object]$Board, [object]$Card, [object]$Result, [string]$Prefix, [int]$TimeoutMs = 6000)
    $menu = Open-CsCardMenu -Board $Board -Card $Card
    if (-not $menu) {
        [void](Add-CsCheck -Result $Result -Name ($Prefix + ' card menu opens') -Pass $false -Detail 'no #32768 popup')
        return $null
    }
    $names = Get-CsMenuNames -Menu $menu.Hwnd
    $Result.Evidence.Add(('{0} card menu items: {1}' -f $Prefix, ($(if ($names) { $names -join ' | ' } else { '<unavailable>' }))))
    if (-not (Wait-CsMenuReady)) {
        Close-CsPopup -Popup $menu
        [void](Add-CsCheck -Result $Result -Name ($Prefix + ' MSAA menu automation is available') `
            -Pass $false -Status 'blocked' -Detail 'Accessibility assembly or STA MSAA helper unavailable')
        return $null
    }
    if (-not (Invoke-CsMenuItem -Menu $menu.Hwnd -Pattern 'Approve HTTP endpoint*' -Result $Result -Label ($Prefix + ' approve'))) {
        Close-CsPopup -Popup $menu
        [void](Add-CsCheck -Result $Result -Name ($Prefix + ' approve menu item invoked') -Pass $false `
            -Detail ('items=' + ($names -join ' | ')))
        return $null
    }
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline) {
        $dialogs = @(Get-DeskDialogList -ProcessId $Board.ProcessId)
        if ($dialogs.Count -gt 0) { return $dialogs[0] }
        Start-Sleep -Milliseconds 100
    }
    return $null
}

function Invoke-CsDialogButton {
    param([IntPtr]$Dialog, [int]$ButtonId)
    Initialize-CsWin32
    $button = [DeskCardspecWin32]::GetDlgItem($Dialog, $ButtonId)
    if ($button -ne [IntPtr]::Zero) {
        [void](Send-CsMessageTimeout -Hwnd $button -Message 0x00F5 -TimeoutMs 4000)  # BM_CLICK
    } else {
        [void](Send-CsMessageTimeout -Hwnd $Dialog -Message 0x0111 -WParam ([IntPtr]$ButtonId) -TimeoutMs 4000)  # WM_COMMAND
    }
    Start-Sleep -Milliseconds 500
}

# ---------------------------------------------------------------------------
# board / card geometry + own-canvas frames
# ---------------------------------------------------------------------------

function Update-CsCard {
    # After a Reload the card height can change and the board re-plans the
    # layout, so the persisted position must be re-read before any pixel ROI is
    # derived from it. The previous geometry is kept only when the file has no
    # usable card entry.
    param([string]$ConfigDir, [object]$Previous)
    $fresh = Get-CsCard -ConfigDir $ConfigDir
    if ($fresh -and $fresh.Visible -eq 1) { return $fresh }
    return $Previous
}

function Read-CsIni {
    # wg::Store saves through a temp file + atomic replace, so a read can very
    # briefly fail with a sharing violation. A failed read returns $null and the
    # caller waits or reports it instead of crashing the suite.
    param([string]$Path)
    try { return (Read-DeskIniFile -Path $Path) } catch { return $null }
}

function Get-CsCard {
    param([string]$ConfigDir)
    $ini = Read-CsIni -Path (Join-Path $ConfigDir 'desk.ini')
    if ($null -eq $ini) { return $null }
    if (-not $ini.Contains('card.cardspec.x') -or -not $ini.Contains('card.cardspec.y')) { return $null }
    return [pscustomobject]@{
        X = [int]$ini['card.cardspec.x']
        Y = [int]$ini['card.cardspec.y']
        W = $script:CardWidth
        Visible = (Get-DeskIniInt $ini 'card.cardspec.visible' 0)
    }
}

function Wait-CsCard {
    param([string]$ConfigDir, [int]$TimeoutSeconds = 10)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $card = Get-CsCard -ConfigDir $ConfigDir
        if ($card -and $card.Visible -eq 1) { return $card }
        Start-Sleep -Milliseconds 200
    }
    return (Get-CsCard -ConfigDir $ConfigDir)
}

function Get-CsFrame {
    # Own-canvas export only (WM_APP+11); nothing is read from the screen.
    param([object]$Run, [object]$Board, [int]$Slot, [string]$Label)
    $frame = Join-Path $Board.ConfigDir ('assistant-test-frame-{0}.bmp' -f $Slot)
    Remove-Item -LiteralPath $frame -Force -ErrorAction SilentlyContinue
    $value = Send-CsMessageTimeout -Hwnd $Board.Hwnd -Message ([uint32]$script:CanvasMessage) `
        -WParam ([IntPtr]$Slot) -TimeoutMs 8000
    if (-not $value.Ok) {
        return [pscustomobject]@{ Ok = $false; Bitmap = $null; Path = ''; Stats = ''; Reason = 'WM_APP+11 timed out (board UI thread did not answer)' }
    }
    if ($value.Result -ne 1) {
        return [pscustomobject]@{ Ok = $false; Bitmap = $null; Path = ''; Stats = ''; Reason = 'WM_APP+11 returned != 1' }
    }
    if (-not (Test-Path -LiteralPath $frame)) {
        return [pscustomobject]@{ Ok = $false; Bitmap = $null; Path = ''; Stats = ''; Reason = 'frame BMP not written' }
    }
    $loaded = $null
    try { $loaded = New-Object System.Drawing.Bitmap($frame) } catch {
        return [pscustomobject]@{ Ok = $false; Bitmap = $null; Path = $frame; Stats = ''; Reason = 'frame BMP unreadable' }
    }
    $copy = New-Object System.Drawing.Bitmap($loaded)
    $stats = [DeskTestImage]::Stats($loaded)
    $loaded.Dispose()
    $png = Join-Path $Run.EvidenceDir ($Label + '.png')
    $copy.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
    return [pscustomobject]@{ Ok = $true; Bitmap = $copy; Path = $png; Stats = $stats; Reason = '' }
}

function Close-CsFrame {
    param([object]$Frame)
    if ($null -ne $Frame -and $null -ne $Frame.Bitmap) {
        $Frame.Bitmap.Dispose()
        $Frame.Bitmap = $null
    }
}

function Get-CsBodyRect {
    param([object]$Card, [int]$CardHeight)
    return [pscustomobject]@{
        X = [int]$Card.X
        Y = [int]$Card.Y + $script:CardHeaderH + $script:CardPadTop
        W = [int]$Card.W
        H = [math]::Max(0, $CardHeight - $script:CardHeaderH - $script:CardPadTop - $script:CardPadBottom)
    }
}

function Get-CsFooterRect {
    # The AS OF band exactly where cardspec_view::DrawBody paints it: below the
    # last row, kAsOfGap under it, one kLineH box. Only the HTTP success layout
    # paints a footer, so the same rectangle on a Blocked/error card must be
    # empty - that pair is the negative/positive control for this ROI.
    param([object]$Card, [int]$CardHeight, [int]$Rows)
    $body = Get-CsBodyRect -Card $Card -CardHeight $CardHeight
    return [pscustomobject]@{
        X = [int]$body.X + $script:CardPadX
        Y = [int]$body.Y + $script:TitleBlock + $Rows * $script:RowPitch + $script:AsOfGap
        W = [int]$Card.W - 2 * $script:CardPadX
        H = $script:LineH
    }
}

function Get-CsFooterInk {
    param([object]$Frame, [object]$Card, [int]$CardHeight, [int]$Rows)
    if (-not $Frame.Ok) { return -1 }
    $rect = Get-CsFooterRect -Card $Card -CardHeight $CardHeight -Rows $Rows
    if ($rect.W -le 0 -or $rect.H -le 0) { return -1 }
    return [DeskTestImage]::CountInkPixels($Frame.Bitmap, $rect.Y, ($rect.Y + $rect.H),
        $rect.X, ($rect.X + $rect.W), 60)
}

function Measure-CsCardHeight {
    # The card's straight bottom border row, found by exact theme-colour counts.
    # The header divider (card.y + 30) has the same colour, so the scan starts at
    # +40; the bar track and the text never reach the border colour.
    param([object]$Frame, [object]$Card)
    if (-not $Frame.Ok) { return -1 }
    if ($Card.Y + 760 -gt $Frame.Bitmap.Height) { return -1 }
    $need = [int][math]::Floor(([int]$Card.W - 80) * 0.8)
    $row = [DeskCardspecImage]::FindRowWithCount($Frame.Bitmap, ([int]$Card.X + 8),
        ([int]$Card.X + [int]$Card.W - 8), ([int]$Card.Y + 40), ([int]$Card.Y + 760),
        $script:ColorBorder[0], $script:ColorBorder[1], $script:ColorBorder[2], $script:ColorTol, $need)
    if ($row -lt 0) { return -1 }
    return ($row - [int]$Card.Y + 1)
}

function Get-CsInkBands {
    # Ink bands (runs of consecutive ink rows) inside a rectangle.
    param([object]$Frame, [int]$X0, [int]$X1, [int]$Y0, [int]$Y1, [int]$MinLum = 60)
    if (-not $Frame.Ok) { return [object[]]@() }
    $rows = [DeskTestImage]::InkRows($Frame.Bitmap, $X0, $X1, $Y0, $Y1, $MinLum)
    $bands = New-Object System.Collections.Generic.List[object]
    $start = -1
    $last = -1
    foreach ($row in @($rows)) {
        if ($start -lt 0) { $start = $row; $last = $row; continue }
        if ($row -eq ($last + 1)) { $last = $row; continue }
        $bands.Add([pscustomobject]@{ Top = $start; Bottom = $last; Height = ($last - $start + 1) })
        $start = $row
        $last = $row
    }
    if ($start -ge 0) { $bands.Add([pscustomobject]@{ Top = $start; Bottom = $last; Height = ($last - $start + 1) }) }
    # Unary comma: a 0- or 1-element result must stay an array, otherwise the
    # caller receives a scalar and ".Count" is empty (PowerShell 5.1).
    return ,([object[]]$bands.ToArray())
}

function Get-CsChipColors {
    # Status chip colours in the card title bar (right-aligned text + dot).
    param([object]$Frame, [object]$Card)
    if (-not $Frame.Ok) {
        return [pscustomobject]@{ Ok = $false; Green = 0; Warn = 0; Bad = 0 }
    }
    $x = [int]$Card.X + 380
    $y = [int]$Card.Y + 8
    $w = [math]::Min(186, [int]$Card.W - 380)
    $green = [DeskCardspecImage]::CountNear($Frame.Bitmap, $x, $y, $w, 22,
        $script:ColorOk[0], $script:ColorOk[1], $script:ColorOk[2], 12)
    $warn = [DeskCardspecImage]::CountNear($Frame.Bitmap, $x, $y, $w, 22,
        $script:ColorWarn[0], $script:ColorWarn[1], $script:ColorWarn[2], 12)
    $bad = [DeskCardspecImage]::CountNear($Frame.Bitmap, $x, $y, $w, 22,
        $script:ColorBad[0], $script:ColorBad[1], $script:ColorBad[2], 12)
    return [pscustomobject]@{ Ok = $true; Green = $green; Warn = $warn; Bad = $bad }
}

function Get-CsChipState {
    param([object]$Colors)
    if (-not $Colors.Ok) { return 'unavailable' }
    # A real chip paints a 3x3 dot plus a 5-8 glyph label at scale 1.0, i.e. far
    # more than 20 matching pixels of its own colour.
    if ($Colors.Green -ge 20 -and $Colors.Green -gt $Colors.Warn -and $Colors.Green -gt $Colors.Bad) { return 'ok' }
    if ($Colors.Warn -ge 20 -and $Colors.Warn -gt $Colors.Bad) { return 'warn' }
    if ($Colors.Bad -ge 20) { return 'bad' }
    return 'unknown'
}

# ---------------------------------------------------------------------------
# fetch counter + network observation
# ---------------------------------------------------------------------------

function Get-CsFetchCount {
    # The fixture rewrites the counter on every transport entry and the host
    # initialises it on startup, so a read can briefly hit a sharing window.
    # Retry a few times before reporting -1 (file missing).
    param([string]$ConfigDir)
    $path = Join-Path $ConfigDir 'cardspec-test-fetch-count.txt'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return -1 }
    for ($attempt = 0; $attempt -lt 8; $attempt++) {
        $text = ''
        try { $text = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::ASCII) } catch { $text = '' }
        $text = $text.Trim()
        if ($text) {
            $value = 0
            if ([int]::TryParse($text, [ref]$value)) { return $value }
        }
        Start-Sleep -Milliseconds 60
    }
    return -1
}

function Wait-CsFetchCount {
    # The fixture writes the counter on every transport entry, then the card
    # commits. Wait for the value to settle so "still 0" is a real observation
    # and not a race against a slow worker.
    param([string]$ConfigDir, [int]$TimeoutMs = 8000, [int]$StableMs = 700)
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    $last = Get-CsFetchCount -ConfigDir $ConfigDir
    $changedAt = Get-Date
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 150
        $now = Get-CsFetchCount -ConfigDir $ConfigDir
        if ($now -ne $last) {
            $last = $now
            $changedAt = Get-Date
            continue
        }
        if (((Get-Date) - $changedAt).TotalMilliseconds -ge $StableMs) { return $now }
    }
    return $last
}

function Test-CsNoOutbound {
    # The fake transport is the only fetch path in this mode, so a successful
    # fetch must not own a TCP socket. Local metadata only, no packets.
    #   * "no matching instance" is the expected empty result -> 0 connections;
    #   * a missing cmdlet / access denied / any other error stays BLOCKED, so an
    #     unavailable API is never reported as "0 connections".
    param([int]$ProcessId)
    try {
        $connections = @(Get-NetTCPConnection -OwningProcess $ProcessId -ErrorAction Stop)
        return [pscustomobject]@{
            Available = $true
            Count = $connections.Count
            States = [string[]]@(@($connections | ForEach-Object { [string]$_.State }))
            Reason = ''
        }
    } catch {
        $failure = $_
        $message = [string]$failure.Exception.Message
        $id = [string]$failure.FullyQualifiedErrorId
        $noInstance = ($id -match '(?i)^CmdletizationQuery_NotFound') -or
                      ($message -match '(?i)No matching\s+MSFT_NetTCPConnection|No MSFT_NetTCPConnection objects found')
        if ($noInstance) {
            return [pscustomobject]@{
                Available = $true
                Count = 0
                States = [string[]]@()
                Reason = 'no matching connection (expected empty result)'
            }
        }
        return [pscustomobject]@{
            Available = $false
            Count = -1
            States = [string[]]@()
            Reason = ($id + ': ' + $message)
        }
    }
}

function Get-CsInkBandsBelow {
    param([object[]]$Bands, [int]$Top)
    # Unary comma keeps 0/1 results as arrays (see Get-CsInkBands).
    return ,([object[]]@($Bands | Where-Object { $_.Top -ge $Top }))
}

# ---------------------------------------------------------------------------
# production-state guards
# ---------------------------------------------------------------------------

function Get-CsPathFingerprint {
    # Names/sizes/mtimes only. Contents are never read, so no config value of a
    # live installation can leak into the evidence.
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { return '<missing>' }
    $base = [System.IO.Path]::GetFullPath($Path)
    $parts = New-Object System.Collections.Generic.List[string]
    $items = @(Get-ChildItem -LiteralPath $base -File -Recurse -Force -ErrorAction SilentlyContinue | Sort-Object FullName)
    foreach ($item in $items) {
        $full = [System.IO.Path]::GetFullPath($item.FullName)
        $rel = $full.Substring($base.Length).TrimStart('\')
        $parts.Add(('{0}|{1}|{2}' -f $rel, $item.Length, $item.LastWriteTimeUtc.Ticks))
    }
    return ($parts.ToArray() -join ';')
}

function Get-CsLiveDeskDirs {
    $entries = New-Object System.Collections.Generic.List[object]
    foreach ($process in @(Get-CimInstance Win32_Process -Filter "Name='desk.exe'" -ErrorAction SilentlyContinue)) {
        $path = ''
        try { $path = [string]$process.ExecutablePath } catch { }
        if (-not $path) { continue }
        $entries.Add([pscustomobject]@{ ProcessId = [int]$process.ProcessId; Directory = (Split-Path -Parent $path) })
    }
    return [object[]]$entries.ToArray()
}

function Get-CsProductionSnapshot {
    $candidates = New-Object System.Collections.Generic.List[object]
    if ($env:LOCALAPPDATA) {
        $candidates.Add([pscustomobject]@{ Label = 'installed exe dir'; Path = (Join-Path $env:LOCALAPPDATA 'Programs\Desktop Desk') })
    }
    $candidates.Add([pscustomobject]@{ Label = 'repo build\bin'; Path = (Join-Path $script:BuildRoot 'bin') })
    $mainRoot = Get-CsMainWorktreeRoot -RepoRoot $script:RepoRoot
    if ($mainRoot) {
        $candidates.Add([pscustomobject]@{ Label = 'main worktree build\bin'; Path = (Join-Path $mainRoot 'build\bin') })
    }
    if ($env:APPDATA) {
        $candidates.Add([pscustomobject]@{ Label = 'APPDATA fallback'; Path = (Join-Path $env:APPDATA 'Desktop Desk') })
    }
    $live = @(Get-CsLiveDeskDirs)
    $snapshot = New-Object System.Collections.Generic.List[object]
    $seen = @{}
    foreach ($candidate in $candidates) {
        $full = [System.IO.Path]::GetFullPath($candidate.Path)
        $key = $full.ToLowerInvariant()
        if ($seen.ContainsKey($key)) { continue }
        $seen[$key] = $true
        $exists = Test-Path -LiteralPath $full -PathType Container
        $count = 0
        if ($exists) {
            $count = @(Get-ChildItem -LiteralPath $full -File -Recurse -Force -ErrorAction SilentlyContinue).Count
        }
        $liveOwner = 0
        foreach ($entry in $live) {
            if ([string]::Equals([string]$entry.Directory, $full, [System.StringComparison]::OrdinalIgnoreCase)) {
                $liveOwner = [int]$entry.ProcessId
                break
            }
        }
        $snapshot.Add([pscustomobject]@{
            Label = $candidate.Label
            Path = $full
            Exists = $exists
            Entries = $count
            LiveOwner = $liveOwner
            Fingerprint = (Get-CsPathFingerprint -Path $full)
        })
    }
    return [object[]]$snapshot.ToArray()
}

function Add-CsProductionChecks {
    param([object]$Result, [object[]]$Before, [object[]]$After)
    $assertable = @($Before | Where-Object { -not $_.LiveOwner })
    $observed = @($assertable | Where-Object { $_.Exists -and $_.Entries -gt 0 })
    $changed = New-Object System.Collections.Generic.List[string]
    $summary = New-Object System.Collections.Generic.List[string]
    foreach ($item in $Before) {
        $match = @($After | Where-Object { $_.Path -eq $item.Path })
        $same = ($match.Count -eq 1 -and $match[0].Fingerprint -eq $item.Fingerprint -and
                 $match[0].Exists -eq $item.Exists)
        if (-not $same -and -not $item.LiveOwner) { $changed.Add($item.Label) }
        $state = 'same'
        if (-not $same) { $state = 'changed' }
        if ($item.LiveOwner) { $state = 'live-owner' }
        $summary.Add(('{0} exists={1} entries={2} {3}' -f $item.Label, $item.Exists, $item.Entries, $state))
    }
    $Result.Evidence.Add(('production config targets: ' + ($summary.ToArray() -join '; ')))
    [void](Add-CsCheck -Result $Result -Name 'isolation: production config metadata is unchanged' `
        -Pass ($observed.Count -gt 0 -and $changed.Count -eq 0) -Detail ($changed.ToArray() -join ','))
    if ($observed.Count -eq 0) {
        [void](Add-CsCheck -Result $Result -Name 'isolation: a production config target existed for comparison' `
            -Pass $false -Status 'blocked' -Detail 'all targets missing or owned by a live production instance')
    }
}

# ---------------------------------------------------------------------------
# negative controls
# ---------------------------------------------------------------------------

function Invoke-CsNegativeSuite {
    param([object]$Run, [object]$Result)
    $cases = @()
    $cases += [pscustomobject]@{
        Tag = 'negative-no-suffix'
        Name = 'negative: cardspec-only without DESK_TEST_INSTANCE exits 2'
        Environment = (Get-CsChildEnvironment -Run $Run -ConfigDir '' -Suffix '')
    }
    $cases += [pscustomobject]@{
        Tag = 'negative-no-config-dir'
        Name = 'negative: cardspec-only without DESK_TEST_CONFIG_DIR exits 2'
        Environment = (Get-CsChildEnvironment -Run $Run -ConfigDir '' -Suffix $Run.SuffixNegA)
    }
    $cases += [pscustomobject]@{
        Tag = 'negative-invalid-config-dir'
        Name = 'negative: cardspec-only with an invalid DESK_TEST_CONFIG_DIR exits 2'
        Environment = (Get-CsChildEnvironment -Run $Run -ConfigDir $Run.ConfigInvalid -Suffix $Run.SuffixNegB)
    }
    $seenClasses = New-Object System.Collections.Generic.List[string]
    foreach ($case in $cases) {
        $entry = Start-CsNegativeInstance -Run $Run -Tag $case.Tag -Environment $case.Environment
        $windows = New-Object System.Collections.Generic.List[string]
        $exited = $false
        $exitCode = $null
        $deadline = (Get-Date).AddSeconds(20)
        while ((Get-Date) -lt $deadline) {
            foreach ($window in @(Get-DeskWindowList -ProcessId $entry.ProcessId)) {
                $windows.Add([string]$window.Class)
                $seenClasses.Add([string]$window.Class)
            }
            $trackedExited = $false
            $trackedCode = 0
            if ([DeskCardspecLaunch]::TryGetExit($entry.ProcessId, [ref]$trackedExited, [ref]$trackedCode)) {
                if ($trackedExited) { $exited = $true; $exitCode = $trackedCode; break }
            }
            if (-not (Test-DeskTestAlive -ProcessId $entry.ProcessId)) {
                if ([DeskCardspecLaunch]::TryGetExit($entry.ProcessId, [ref]$trackedExited, [ref]$trackedCode)) {
                    $exited = $true
                    $exitCode = $trackedCode
                }
                break
            }
            Start-Sleep -Milliseconds 100
        }
        $uniqueWindows = [string[]]@(@($windows | Select-Object -Unique))
        $codeText = 'unknown'
        if ($null -ne $exitCode) { $codeText = [string]$exitCode }
        [void](Add-CsCheck -Result $Result -Name $case.Name `
            -Pass ($exited -and $exitCode -eq 2) -Detail ('exited=' + $exited + ' code=' + $codeText))
        [void](Add-CsCheck -Result $Result -Name ($case.Tag + ' opens no window') `
            -Pass ($uniqueWindows.Count -eq 0) -Detail ('classes=' + ($uniqueWindows -join ',')))
        if ($null -ne $entry.Record) {
            $state = Stop-CsInstance -Recorded $entry.Record -Run $Run
            [void](Add-CsCheck -Result $Result -Name ($case.Tag + ' leaves no process') `
                -Pass ($state -eq 'gone' -or $state -eq 'stopped') -Detail ('final=' + $state))
            [DeskCardspecLaunch]::ForgetTracked($entry.ProcessId)
        }
    }
    $productionClasses = @($seenClasses | Where-Object { $_ -eq 'wg.desk' })
    [void](Add-CsCheck -Result $Result -Name 'negative: production window class wg.desk is never created' `
        -Pass ($productionClasses.Count -eq 0) -Detail ('observed=' + ((@($seenClasses | Select-Object -Unique)) -join ',')))
}

# ---------------------------------------------------------------------------
# suite A: inline rows / bar / error / recovery / restart
# ---------------------------------------------------------------------------

function Invoke-CsInlineSuite {
    param([object]$Run, [object]$Result)
    $configDir = $Run.ConfigInline
    $prefix = 'inline'
    $rows2 = @(
        [ordered]@{ label = '学习'; text = '阅读三十分钟' },
        [ordered]@{ label = '任务'; text = '完成两项检查' }
    )
    $rows4 = @(
        [ordered]@{ label = '学习'; text = '阅读三十分钟' },
        [ordered]@{ label = '任务'; text = '完成两项检查' },
        [ordered]@{ label = '记录'; text = '写入一条日志' },
        [ordered]@{ label = '复盘'; text = '整理三个结论' }
    )
    Write-CsSpecFile -ConfigDir $configDir -Spec (New-CsSpecRows -Title '今日目标' -Rows $rows2)

    $board = Start-CsBoard -Run $Run -ConfigDir $configDir -Suffix $Run.SuffixInline -Tag 'inline-board'
    $window = Wait-CsBoardWindow -Board $board -TimeoutSeconds 25
    if (-not $window) {
        [void](Save-CsWindowDiagnostic -Run $Run -Board $board -Path (Join-Path $Run.EvidenceDir 'inline-no-window.json'))
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' isolated board window appears') -Pass $false `
            -Detail ('class=' + $board.ClassName + ' pid=' + $board.ProcessId))
        Close-CsBoard -Run $Run -Board $board -Result $Result -Prefix $prefix
        return
    }
    [void](Add-CsCheck -Result $Result -Name ($prefix + ' isolated board window appears') -Pass $true `
        -Detail ('class=' + $board.ClassName + ' rect=' + $window.W + 'x' + $window.H))

    # (1) real Add module menu -> card on the board, persisted in desk.ini.
    $added = Add-CsCardspecModule -Board $board -Result $Result -Prefix $prefix
    $card = $null
    if ($added) { $card = Wait-CsCard -ConfigDir $configDir -TimeoutSeconds 10 }
    $cardDetail = 'no card.cardspec.* in desk.ini'
    if ($card) { $cardDetail = 'x=' + $card.X + ' y=' + $card.Y + ' visible=' + $card.Visible }
    [void](Add-CsCheck -Result $Result -Name ($prefix + ' CARDSPEC lands on the board and is persisted') `
        -Pass ($null -ne $card -and $card.Visible -eq 1 -and $card.X -ge 0 -and $card.Y -ge 0) -Detail $cardDetail)
    if ($null -eq $card) {
        Close-CsBoard -Run $Run -Board $board -Result $Result -Prefix $prefix
        return
    }
    Start-Sleep -Milliseconds $SettleMs

    # (2) rows2 painting: own-canvas ink, measured card height, ink bands, chip.
    $frame2 = Get-CsFrame -Run $Run -Board $board -Slot 0 -Label 'inline-rows2'
    if (-not $frame2.Ok) {
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' own-canvas frame available') -Pass $false `
            -Status 'blocked' -Detail $frame2.Reason)
        Close-CsFrame -Frame $frame2
        Close-CsBoard -Run $Run -Board $board -Result $Result -Prefix $prefix
        return
    }
    [void](Add-CsCheck -Result $Result -Name ($prefix + ' own-canvas frame available') -Pass $true -Detail $frame2.Stats)
    $height2 = Measure-CsCardHeight -Frame $frame2 -Card $card
    $body2 = Get-CsBodyRect -Card $card -CardHeight $(if ($height2 -gt 0) { $height2 } else { $script:WideMinHeight })
    $bands2 = Get-CsInkBands -Frame $frame2 -X0 ($card.X + $script:CardPadX) -X1 ($card.X + $card.W - $script:CardPadX) `
        -Y0 $body2.Y -Y1 ($body2.Y + $body2.H)
    $chip2 = Get-CsChipColors -Frame $frame2 -Card $card
    [void](Add-CsCheck -Result $Result -Name ($prefix + ' rows2 card height follows the Wide preset minimum') `
        -Pass ($height2 -eq $script:WideMinHeight) `
        -Detail ('measured=' + $height2 + ' expected=168 (body 92 + 48, clamped to Wide min 168)'))
    [void](Add-CsCheck -Result $Result -Name ($prefix + ' rows2 paints title + 2 Chinese rows') `
        -Pass ($bands2.Count -ge 5) -Detail ('ink bands=' + $bands2.Count + ' tops=' + (($bands2 | ForEach-Object { $_.Top }) -join ',')))
    [void](Add-CsCheck -Result $Result -Name ($prefix + ' rows2 shows the LIVE chip') `
        -Pass ((Get-CsChipState -Colors $chip2) -eq 'ok') -Detail ('green=' + $chip2.Green + ' warn=' + $chip2.Warn + ' bad=' + $chip2.Bad))
    $Result.Evidence.Add(('inline rows2: height={0} bands={1} green={2}' -f $height2, $bands2.Count, $chip2.Green))

    # (3) editing the JSON must not change the card within 3 seconds.
    Write-CsSpecFile -ConfigDir $configDir -Spec (New-CsSpecRows -Title '今日目标' -Rows $rows4)
    Start-Sleep -Seconds 3
    $frameIdle = Get-CsFrame -Run $Run -Board $board -Slot 1 -Label 'inline-rows4-no-autorefresh'
    $diffIdle = -1.0
    $roiH = 0
    $roiValid = $false
    if ($frameIdle.Ok) {
        $roiH = [math]::Min([int]$height2,
            [math]::Min($frame2.Bitmap.Height, $frameIdle.Bitmap.Height) - [int]$card.Y)
        if ($roiH -gt 0 -and [int]$card.W -gt 0 -and [int]$card.Y -ge 0) {
            $roiValid = $true
            $diffIdle = [DeskTestImage]::DiffFraction($frame2.Bitmap, $frameIdle.Bitmap, [int]$card.X, [int]$card.Y,
                [int]$card.W, $roiH, 12)
        }
    }
    # Unchanged content must diff to exactly 0; a 0 here is the passing value (an
    # empty/negative ROI is not evidence, so roiH > 0 is required too).
    [void](Add-CsCheck -Result $Result -Name ($prefix + ' editing cardspec.json does not auto-refresh within 3s') `
        -Pass ($frameIdle.Ok -and $roiValid -and $diffIdle -ge 0.0 -and $diffIdle -lt 0.002) `
        -Detail ('roiH=' + $roiH + ' valid=' + $roiValid + ' card ROI diff fraction=' + $diffIdle + ' (threshold 0.002)'))
    Close-CsFrame -Frame $frameIdle

    # (4) the card menu Reload applies the new document (manual refresh only).
    $countBefore = Wait-CsFetchCount -ConfigDir $configDir
    $menuNames = Invoke-CsCardMenuItem -Board $board -Card $card -Pattern 'Reload cardspec.json*' `
        -Result $Result -Prefix ($prefix + ' reload-rows4')
    $reloadOk = $false
    if ($null -eq $menuNames) {
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' card menu exposes Reload cardspec.json') `
            -Pass $false -Status 'blocked' -Detail 'MSAA menu labels unavailable; keyboard fallback was used instead')
    } else {
        $reloadOk = (@($menuNames | Where-Object { $_ -like 'Reload cardspec.json*' }).Count -eq 1)
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' card menu exposes Reload cardspec.json') -Pass $reloadOk `
            -Detail ($menuNames -join ' | '))
    }
    Start-Sleep -Milliseconds $SettleMs
    $card = Update-CsCard -ConfigDir $configDir -Previous $card
    $frame4 = Get-CsFrame -Run $Run -Board $board -Slot 0 -Label 'inline-rows4-applied'
    $height4 = -1
    $bands4 = [object[]]@()
    if ($frame4.Ok) {
        $height4 = Measure-CsCardHeight -Frame $frame4 -Card $card
        $body4 = Get-CsBodyRect -Card $card -CardHeight $(if ($height4 -gt 0) { $height4 } else { 208 })
        $bands4 = Get-CsInkBands -Frame $frame4 -X0 ($card.X + $script:CardPadX) -X1 ($card.X + $card.W - $script:CardPadX) `
            -Y0 $body4.Y -Y1 ($body4.Y + $body4.H)
    }
    [void](Add-CsCheck -Result $Result -Name ($prefix + ' manual Reload applies rows4 (card height 208)') `
        -Pass ($height4 -eq 208) -Detail ('measured=' + $height4 + ' expected=208 (body 160 + 48)'))
    [void](Add-CsCheck -Result $Result -Name ($prefix + ' rows4 paints 4 rows and reflows taller') `
        -Pass ($bands4.Count -ge 9 -and $bands4.Count -gt $bands2.Count) `
        -Detail ('ink bands=' + $bands4.Count + ' (rows2 was ' + $bands2.Count + ')'))
    $countAfterReload = Wait-CsFetchCount -ConfigDir $configDir
    [void](Add-CsCheck -Result $Result -Name ($prefix + ' inline Reload performs no fetch') `
        -Pass ($countBefore -eq 0 -and $countAfterReload -eq 0) -Detail ('count ' + $countBefore + ' -> ' + $countAfterReload))
    Close-CsFrame -Frame $frame2
    Close-CsFrame -Frame $frame4

    # (5) bar 3/5 vs 0/5 pixel fill on the board.
    Write-CsSpecFile -ConfigDir $configDir -Spec (New-CsSpecBar -Title '目标进度' -Value 3 -Max 5)
    [void](Invoke-CsCardMenuItem -Board $board -Card $card -Pattern 'Reload cardspec.json*' -Result $Result -Prefix ($prefix + ' reload-bar3'))
    Start-Sleep -Milliseconds $SettleMs
    $card = Update-CsCard -ConfigDir $configDir -Previous $card
    $frameBar3 = Get-CsFrame -Run $Run -Board $board -Slot 0 -Label 'inline-bar-3-of-5'
    Write-CsSpecFile -ConfigDir $configDir -Spec (New-CsSpecBar -Title '目标进度' -Value 0 -Max 5)
    [void](Invoke-CsCardMenuItem -Board $board -Card $card -Pattern 'Reload cardspec.json*' -Result $Result -Prefix ($prefix + ' reload-bar0'))
    Start-Sleep -Milliseconds $SettleMs
    $card = Update-CsCard -ConfigDir $configDir -Previous $card
    $frameBar0 = Get-CsFrame -Run $Run -Board $board -Slot 1 -Label 'inline-bar-0-of-5'
    $barHeight = -1
    $barDiff = -1
    $green3 = 0
    $green0 = 0
    if ($frameBar3.Ok -and $frameBar0.Ok) {
        $barHeight = Measure-CsCardHeight -Frame $frameBar0 -Card $card
        $bodyBar = Get-CsBodyRect -Card $card -CardHeight $(if ($barHeight -gt 0) { $barHeight } else { $script:WideMinHeight })
        $barTop = [int]$bodyBar.Y + $script:TitleBlock + $script:BarOffsetY
        $barGreen3 = [DeskCardspecImage]::CountNear($frameBar3.Bitmap, [int]$bodyBar.X + $script:CardPadX, $barTop,
            ([int]$card.W - 2 * $script:CardPadX), $script:BarH, $script:ColorOk[0], $script:ColorOk[1], $script:ColorOk[2], 12)
        $barGreen0 = [DeskCardspecImage]::CountNear($frameBar0.Bitmap, [int]$bodyBar.X + $script:CardPadX, $barTop,
            ([int]$card.W - 2 * $script:CardPadX), $script:BarH, $script:ColorOk[0], $script:ColorOk[1], $script:ColorOk[2], 12)
        $green3 = $barGreen3
        $green0 = $barGreen0
        $barDiff = [DeskCardspecImage]::ColumnDiff($frameBar3.Bitmap, $frameBar0.Bitmap,
            [int]$bodyBar.X + $script:CardPadX, [int]$bodyBar.X + [int]$card.W - $script:CardPadX,
            $barTop, ($barTop + $script:BarH), 30)
    }
    # 544 * 3/5 = 326.4 -> Canvas::Bar truncates to 326; 0/5 paints only the track.
    [void](Add-CsCheck -Result $Result -Name ($prefix + ' bar 3/5 fills ~326 of 544 px') `
        -Pass ($barDiff -ge 322 -and $barDiff -le 330 -and $green3 -ge 300) `
        -Detail ('differing columns=' + $barDiff + ' green(3/5)=' + $green3))
    [void](Add-CsCheck -Result $Result -Name ($prefix + ' bar 0/5 paints the empty track only') `
        -Pass ($green0 -eq 0 -and $barDiff -ge 0) -Detail ('green(0/5)=' + $green0))
    Close-CsFrame -Frame $frameBar3
    Close-CsFrame -Frame $frameBar0

    # (6) an invalid document must clear the old rows and show INVALID.
    Write-CsFile -Path (Join-Path $configDir 'cardspec.json') `
        -Text '{"version":1,"title":"坏规格","source":{"type":"inline"},"layout":"rows","rows":[{"label":"缺少文本"}]}'
    [void](Invoke-CsCardMenuItem -Board $board -Card $card -Pattern 'Reload cardspec.json*' -Result $Result -Prefix ($prefix + ' reload-invalid'))
    Start-Sleep -Milliseconds $SettleMs
    $card = Update-CsCard -ConfigDir $configDir -Previous $card
    $frameInvalid = Get-CsFrame -Run $Run -Board $board -Slot 0 -Label 'inline-invalid-json'
    $invalidOk = $false
    $invalidDetail = 'frame unavailable'
    if ($frameInvalid.Ok) {
        $heightInvalid = Measure-CsCardHeight -Frame $frameInvalid -Card $card
        $bodyInvalid = Get-CsBodyRect -Card $card -CardHeight $(if ($heightInvalid -gt 0) { $heightInvalid } else { $script:WideMinHeight })
        $bandsInvalid = Get-CsInkBands -Frame $frameInvalid -X0 ($card.X + $script:CardPadX) -X1 ($card.X + $card.W - $script:CardPadX) `
            -Y0 $bodyInvalid.Y -Y1 ($bodyInvalid.Y + $bodyInvalid.H)
        $below = Get-CsInkBandsBelow -Bands $bandsInvalid -Top ([int]$bodyInvalid.Y + 90)
        $chipInvalid = Get-CsChipColors -Frame $frameInvalid -Card $card
        $invalidState = Get-CsChipState -Colors $chipInvalid
        $invalidOk = ($below.Count -eq 0 -and $invalidState -eq 'bad' -and $bandsInvalid.Count -ge 2)
        $invalidDetail = ('bands=' + $bandsInvalid.Count + ' below+90=' + $below.Count + ' chip=' + $invalidState)
    }
    [void](Add-CsCheck -Result $Result -Name ($prefix + ' invalid JSON clears the old rows and shows INVALID') `
        -Pass $invalidOk -Detail $invalidDetail)
    Close-CsFrame -Frame $frameInvalid

    # (7) a missing document still shows the error card, not stale rows.
    Remove-Item -LiteralPath (Join-Path $configDir 'cardspec.json') -Force -ErrorAction SilentlyContinue
    [void](Invoke-CsCardMenuItem -Board $board -Card $card -Pattern 'Reload cardspec.json*' -Result $Result -Prefix ($prefix + ' reload-missing'))
    Start-Sleep -Milliseconds $SettleMs
    $card = Update-CsCard -ConfigDir $configDir -Previous $card
    $frameMissing = Get-CsFrame -Run $Run -Board $board -Slot 1 -Label 'inline-missing-json'
    $missingOk = $false
    $missingDetail = 'frame unavailable'
    if ($frameMissing.Ok) {
        $heightMissing = Measure-CsCardHeight -Frame $frameMissing -Card $card
        $bodyMissing = Get-CsBodyRect -Card $card -CardHeight $(if ($heightMissing -gt 0) { $heightMissing } else { $script:WideMinHeight })
        $bandsMissing = Get-CsInkBands -Frame $frameMissing -X0 ($card.X + $script:CardPadX) -X1 ($card.X + $card.W - $script:CardPadX) `
            -Y0 $bodyMissing.Y -Y1 ($bodyMissing.Y + $bodyMissing.H)
        $belowMissing = Get-CsInkBandsBelow -Bands $bandsMissing -Top ([int]$bodyMissing.Y + 90)
        $chipMissing = Get-CsChipColors -Frame $frameMissing -Card $card
        $missingState = Get-CsChipState -Colors $chipMissing
        $missingOk = ($belowMissing.Count -eq 0 -and $missingState -eq 'warn' -and $bandsMissing.Count -ge 2)
        $missingDetail = ('bands=' + $bandsMissing.Count + ' below+90=' + $belowMissing.Count + ' chip=' + $missingState)
    }
    [void](Add-CsCheck -Result $Result -Name ($prefix + ' missing cardspec.json shows NO SPEC without old rows') `
        -Pass $missingOk -Detail $missingDetail)
    Close-CsFrame -Frame $frameMissing

    # (8) repair in place, no restart: Reload restores the content.
    Write-CsSpecFile -ConfigDir $configDir -Spec (New-CsSpecRows -Title '今日目标' -Rows $rows4)
    [void](Invoke-CsCardMenuItem -Board $board -Card $card -Pattern 'Reload cardspec.json*' -Result $Result -Prefix ($prefix + ' reload-repaired'))
    Start-Sleep -Milliseconds $SettleMs
    $card = Update-CsCard -ConfigDir $configDir -Previous $card
    $frameRepair = Get-CsFrame -Run $Run -Board $board -Slot 0 -Label 'inline-repaired'
    $repairOk = $false
    $repairDetail = 'frame unavailable'
    if ($frameRepair.Ok) {
        $heightRepair = Measure-CsCardHeight -Frame $frameRepair -Card $card
        $bodyRepair = Get-CsBodyRect -Card $card -CardHeight $(if ($heightRepair -gt 0) { $heightRepair } else { 208 })
        $bandsRepair = Get-CsInkBands -Frame $frameRepair -X0 ($card.X + $script:CardPadX) -X1 ($card.X + $card.W - $script:CardPadX) `
            -Y0 $bodyRepair.Y -Y1 ($bodyRepair.Y + $bodyRepair.H)
        $repairOk = ($heightRepair -eq 208 -and $bandsRepair.Count -ge 9)
        $repairDetail = ('height=' + $heightRepair + ' bands=' + $bandsRepair.Count)
    }
    [void](Add-CsCheck -Result $Result -Name ($prefix + ' repaired JSON recovers without a restart') -Pass $repairOk -Detail $repairDetail)
    Close-CsFrame -Frame $frameRepair

    # (9) restart: visibility/content come from the isolated config, no fetch.
    $visibleBefore = $card.Visible
    Close-CsBoard -Run $Run -Board $board -Result $Result -Prefix $prefix
    Start-Sleep -Milliseconds 800
    $restarted = Start-CsBoard -Run $Run -ConfigDir $configDir -Suffix $Run.SuffixInline -Tag 'inline-board-restart'
    $windowRestart = Wait-CsBoardWindow -Board $restarted -TimeoutSeconds 25
    [void](Add-CsCheck -Result $Result -Name ($prefix + ' restart shows the board again') -Pass ($null -ne $windowRestart) `
        -Detail ('class=' + $restarted.ClassName))
    if ($windowRestart) {
        Start-Sleep -Milliseconds $SettleMs
        $cardRestart = Wait-CsCard -ConfigDir $configDir -TimeoutSeconds 10
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' restart keeps CARDSPEC visible without Add module') `
            -Pass ($null -ne $cardRestart -and $cardRestart.Visible -eq 1 -and $visibleBefore -eq 1) `
            -Detail $(if ($cardRestart) { 'visible=' + $cardRestart.Visible + ' x=' + $cardRestart.X + ' y=' + $cardRestart.Y } else { 'card missing' }))
        $frameRestart = Get-CsFrame -Run $Run -Board $restarted -Slot 0 -Label 'inline-after-restart'
        $restartOk = $false
        $restartDetail = 'frame unavailable'
        if ($frameRestart.Ok -and $cardRestart) {
            $heightRestart = Measure-CsCardHeight -Frame $frameRestart -Card $cardRestart
            $bodyRestart = Get-CsBodyRect -Card $cardRestart -CardHeight $(if ($heightRestart -gt 0) { $heightRestart } else { 208 })
            $bandsRestart = Get-CsInkBands -Frame $frameRestart -X0 ($cardRestart.X + $script:CardPadX) -X1 ($cardRestart.X + $cardRestart.W - $script:CardPadX) `
                -Y0 $bodyRestart.Y -Y1 ($bodyRestart.Y + $bodyRestart.H)
            $restartOk = ($bandsRestart.Count -ge 9)
            $restartDetail = ('height=' + $heightRestart + ' bands=' + $bandsRestart.Count)
        }
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' restart loads the inline spec again') -Pass $restartOk -Detail $restartDetail)
        Close-CsFrame -Frame $frameRestart
        $countRestart = Wait-CsFetchCount -ConfigDir $configDir
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' restart performs no fetch (counter reset to 0)') `
            -Pass ($countRestart -eq 0) -Detail ('count=' + $countRestart))
    }
    Close-CsBoard -Run $Run -Board $restarted -Result $Result -Prefix ($prefix + '-restart')
}

function Get-CsCardMenuNames {
    # Opens the real card menu, records the item labels, then dismisses it. The
    # labels are the permission state evidence ("Approve HTTP endpoint..." vs
    # "Revoke HTTP approval") without any OCR of the painted card.
    param([object]$Board, [object]$Card)
    $menu = Open-CsCardMenu -Board $Board -Card $Card
    if (-not $menu) { return $null }
    $names = Get-CsMenuNames -Menu $menu.Hwnd
    Close-CsPopup -Popup $menu
    Start-Sleep -Milliseconds 250
    return [string[]]$names
}

function Test-CsApproveDialog {
    # No button may be pressed before the box is proven to be the product's own
    # confirmation: exact title, both IDOK and IDCANCEL present, and
    # DM_GETDEFID reporting Cancel (MB_DEFBUTTON2) as the default, so even a
    # stray Enter cannot approve.
    param([object]$Dialog)
    Initialize-CsWin32
    $title = ''
    if ($null -ne $Dialog) { $title = [string]$Dialog.Title }
    $titleOk = ($title -eq 'CARDSPEC · 网络授权')
    $okButton = [IntPtr]::Zero
    $cancelButton = [IntPtr]::Zero
    if ($null -ne $Dialog) {
        $okButton = [DeskCardspecWin32]::GetDlgItem($Dialog.Hwnd, 1)     # IDOK
        $cancelButton = [DeskCardspecWin32]::GetDlgItem($Dialog.Hwnd, 2) # IDCANCEL
    }
    $buttonsOk = ($okButton -ne [IntPtr]::Zero -and $cancelButton -ne [IntPtr]::Zero)
    $defaultKnown = $false
    $defaultId = -1
    if ($null -ne $Dialog) {
        $def = Send-CsMessageTimeout -Hwnd $Dialog.Hwnd -Message 0x0400 -TimeoutMs 3000   # DM_GETDEFID
        if ($def.Ok -and (((([int64]$def.Result) -shr 16) -band 0xFFFF) -eq 0x534B)) {     # DC_HASDEFID
            $defaultKnown = $true
            $defaultId = [int](([int64]$def.Result) -band 0xFFFF)
        }
    }
    $defaultOk = ($defaultKnown -and $defaultId -eq 2)
    $detail = 'title=' + $(if ($titleOk) { 'exact' } else { '"' + $title + '"' }) +
        ' ok-button=' + ($okButton -ne [IntPtr]::Zero) +
        ' cancel-button=' + ($cancelButton -ne [IntPtr]::Zero) +
        ' default=' + $(if ($defaultKnown) { [string]$defaultId } else { 'unknown' })
    return [pscustomobject]@{
        Ok = ($titleOk -and $buttonsOk -and $defaultOk)
        TitleOk = $titleOk
        ButtonsOk = $buttonsOk
        DefaultOk = $defaultOk
        Detail = $detail
    }
}

# ---------------------------------------------------------------------------
# suite B: v2 HTTP with the fixed native fake transport
# ---------------------------------------------------------------------------

function Add-CsHttpRenderCheck {
    # Painted evidence for a successful fake fetch: HTTP chip (green), the bound
    # rows, and ink inside the exact AS OF footer rectangle. No OCR is attempted:
    # the fixed fixture body itself is asserted statically against the image, and
    # the footer ROI is paired with the empty-footer negative control taken on
    # the blocked card.
    param([object]$Run, [object]$Board, [object]$Card, [object]$Result, [string]$Name, [string]$Slot = '1', [int]$Rows = 2)
    $frame = Get-CsFrame -Run $Run -Board $Board -Slot ([int]$Slot) -Label ('http-render-' + ($Name -replace '[^A-Za-z0-9]+', '-'))
    $ok = $false
    $detail = 'frame unavailable'
    if ($frame.Ok) {
        $height = Measure-CsCardHeight -Frame $frame -Card $Card
        $cardHeight = $script:WideMinHeight
        if ($height -gt 0) { $cardHeight = $height }
        $expectedBands = 1 + 2 * $Rows
        $body = Get-CsBodyRect -Card $Card -CardHeight $cardHeight
        $bands = Get-CsInkBands -Frame $frame -X0 ($Card.X + $script:CardPadX) -X1 ($Card.X + $Card.W - $script:CardPadX) `
            -Y0 ([int]$body.Y) -Y1 ([int]$body.Y + [int]$body.H)
        $footer = Get-CsFooterRect -Card $Card -CardHeight $cardHeight -Rows $Rows
        $footerInk = Get-CsFooterInk -Frame $frame -Card $Card -CardHeight $cardHeight -Rows $Rows
        $chip = Get-CsChipState -Colors (Get-CsChipColors -Frame $frame -Card $Card)
        $ok = ($chip -eq 'ok' -and $bands.Count -ge $expectedBands -and $footerInk -ge 20)
        $detail = ('chip=' + $chip + ' ink bands=' + $bands.Count + ' (>= ' + $expectedBands + ') footer ink=' +
            $footerInk + ' in y=' + $footer.Y + '..' + ($footer.Y + $footer.H - 1) + ' height=' + $height)
    }
    Close-CsFrame -Frame $frame
    [void](Add-CsCheck -Result $Result -Name $Name -Pass $ok -Detail $detail)
    return $ok
}

function Invoke-CsHttpSuite {
    param([object]$Run, [object]$Result)
    $configDir = $Run.ConfigHttp
    $prefix = 'http'
    $profile = 'demo'
    $url = 'https://example.com/cardspec-fixture/data'
    $iniPath = Join-Path $configDir 'cardspec.ini'
    Write-CsIni -Path $iniPath -Values ([ordered]@{
        'http.profile' = $profile
        'http.url' = $url
        'http.approved_profile' = ''
        'http.approved_url' = ''
    })
    Write-CsSpecFile -ConfigDir $configDir -Spec (New-CsSpecHttpRows -Title '公开仓库' -Profile $profile)
    $Result.Evidence.Add('http fixture: fixed native fake transport, public HTTPS URL is never resolved')

    $board = Start-CsBoard -Run $Run -ConfigDir $configDir -Suffix $Run.SuffixHttp -Tag 'http-board'
    $window = Wait-CsBoardWindow -Board $board -TimeoutSeconds 25
    if (-not $window) {
        [void](Save-CsWindowDiagnostic -Run $Run -Board $board -Path (Join-Path $Run.EvidenceDir 'http-no-window.json'))
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' isolated board window appears') -Pass $false `
            -Detail ('class=' + $board.ClassName + ' pid=' + $board.ProcessId))
        Close-CsBoard -Run $Run -Board $board -Result $Result -Prefix $prefix
        return
    }
    [void](Add-CsCheck -Result $Result -Name ($prefix + ' isolated board window appears') -Pass $true `
        -Detail ('class=' + $board.ClassName + ' rect=' + $window.W + 'x' + $window.H))

    [void](Add-CsCardspecModule -Board $board -Result $Result -Prefix $prefix)
    $card = Wait-CsCard -ConfigDir $configDir -TimeoutSeconds 10
    if ($null -eq $card) {
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' CARDSPEC lands on the board') -Pass $false -Detail 'no card.cardspec.* in desk.ini')
        Close-CsBoard -Run $Run -Board $board -Result $Result -Prefix $prefix
        return
    }
    [void](Add-CsCheck -Result $Result -Name ($prefix + ' CARDSPEC lands on the board') -Pass $true `
        -Detail ('x=' + $card.X + ' y=' + $card.Y + ' visible=' + $card.Visible))
    Start-Sleep -Milliseconds $SettleMs

    # (1) unapproved v2: BLOCKED, zero fetches, no grant written.
    $count0 = Wait-CsFetchCount -ConfigDir $configDir
    $frameBlocked = Get-CsFrame -Run $Run -Board $board -Slot 0 -Label 'http-blocked-initial'
    $blockedChip = 'unavailable'
    $blockedFooterInk = -1
    $blockedFooterY = -1
    if ($frameBlocked.Ok) {
        $blockedHeight = Measure-CsCardHeight -Frame $frameBlocked -Card $card
        $blockedCardHeight = $script:WideMinHeight
        if ($blockedHeight -gt 0) { $blockedCardHeight = $blockedHeight }
        $blockedChip = Get-CsChipState -Colors (Get-CsChipColors -Frame $frameBlocked -Card $card)
        $blockedFooter = Get-CsFooterRect -Card $card -CardHeight $blockedCardHeight -Rows 2
        $blockedFooterY = [int]$blockedFooter.Y
        $blockedFooterInk = Get-CsFooterInk -Frame $frameBlocked -Card $card -CardHeight $blockedCardHeight -Rows 2
    }
    # Negative control for the AS OF ROI: while the card is Blocked the footer
    # rectangle must be empty, so the positive footer ink later cannot come from
    # row text or the card chrome.
    [void](Add-CsCheck -Result $Result -Name ($prefix + ' AS OF footer band is empty while blocked (negative control)') `
        -Pass ($blockedFooterInk -eq 0) `
        -Detail ('footer ink=' + $blockedFooterInk + ' at y=' + $blockedFooterY))
    $namesUnapproved = Get-CsCardMenuNames -Board $board -Card $card
    $offersApprove = ($null -ne $namesUnapproved -and @($namesUnapproved | Where-Object { $_ -like 'Approve HTTP endpoint*' }).Count -eq 1)
    $offersRevoke = ($null -ne $namesUnapproved -and @($namesUnapproved | Where-Object { $_ -like 'Revoke HTTP approval*' }).Count -eq 1)
    $Result.Evidence.Add(('http unapproved card menu: ' + $(if ($namesUnapproved) { $namesUnapproved -join ' | ' } else { '<unavailable>' })))
    [void](Add-CsCheck -Result $Result -Name ($prefix + ' unapproved v2 shows BLOCKED (warn chip)') `
        -Pass ($blockedChip -eq 'warn') -Detail ('chip=' + $blockedChip))
    if ($null -eq $namesUnapproved) {
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' unapproved v2 offers Approve and no Revoke') `
            -Pass $false -Status 'blocked' -Detail 'MSAA menu labels unavailable')
    } else {
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' unapproved v2 offers Approve and no Revoke') `
            -Pass ($offersApprove -and -not $offersRevoke) `
            -Detail ('approve=' + $offersApprove + ' revoke=' + $offersRevoke))
    }
    $ini0 = Read-CsIni -Path $iniPath
    $grantEmpty0 = ($null -ne $ini0 -and [string]::IsNullOrEmpty([string]$ini0['http.approved_profile']) -and
                    [string]::IsNullOrEmpty([string]$ini0['http.approved_url']))
    [void](Add-CsCheck -Result $Result -Name ($prefix + ' initial Load leaves the grant empty and fetches 0') `
        -Pass ($count0 -eq 0 -and $grantEmpty0) -Detail ('fetch count=' + $count0 + ' grant-empty=' + $grantEmpty0))
    Close-CsFrame -Frame $frameBlocked

    # (2) the real approve confirmation (MB_OKCANCEL): Cancel first, then OK.
    $dialog = Start-CsApproveCommand -Board $board -Card $card -Result $Result -Prefix ($prefix + ' approve-cancel')
    $dialogSeen = ($null -ne $dialog)
    $Result.Evidence.Add(('http approve confirmation dialog seen: ' + $dialogSeen))
    if ($dialogSeen) {
        $dialogCheck = Test-CsApproveDialog -Dialog $dialog
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' approve opens the product confirmation box') `
            -Pass $dialogCheck.Ok -Detail ('class=' + $dialog.Class + ' ' + $dialogCheck.Detail))
        if ($dialogCheck.Ok) {
            Invoke-CsDialogButton -Dialog $dialog.Hwnd -ButtonId 2   # IDCANCEL
        } else {
            # Never press an unverified box; close it so the board can continue.
            [void](Send-CsMessageTimeout -Hwnd $dialog.Hwnd -Message 0x0010 -TimeoutMs 4000)  # WM_CLOSE
        }
        Start-Sleep -Milliseconds 900
        $countCancel = Wait-CsFetchCount -ConfigDir $configDir
        $iniCancel = Read-CsIni -Path $iniPath
        $grantEmptyCancel = ($null -ne $iniCancel -and [string]::IsNullOrEmpty([string]$iniCancel['http.approved_profile']) -and
                             [string]::IsNullOrEmpty([string]$iniCancel['http.approved_url']))
        if ($dialogCheck.Ok) {
            [void](Add-CsCheck -Result $Result -Name ($prefix + ' cancel writes no grant and fetches 0') `
                -Pass ($grantEmptyCancel -and $countCancel -eq 0) `
                -Detail ('grant-empty=' + $grantEmptyCancel + ' fetch count=' + $countCancel))
        } else {
            [void](Add-CsCheck -Result $Result -Name ($prefix + ' cancel writes no grant and fetches 0') `
                -Pass $false -Detail 'the box was not the expected product dialog, so Cancel was never pressed')
        }
    } else {
        $missingConfirm = 'no #32770 dialog appeared: the cardspec-only fixture must inject the product ' +
            'DefaultConfirm (MessageBoxW MB_OKCANCEL). A stale build or a fixture that lost that callback ' +
            'cannot exercise the real cancel/OK path; the board only reports the missing confirm entry.'
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' approve opens the product confirmation box') `
            -Pass $false -Status 'blocked' -Detail $missingConfirm)
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' cancel writes no grant and fetches 0') `
            -Pass $false -Status 'blocked' -Detail 'the confirmation dialog was never shown, so cancel cannot be exercised')
    }

    $fetchPositive = $false
    $seededGrant = $false
    if ($dialogSeen) {
        $dialog2 = Start-CsApproveCommand -Board $board -Card $card -Result $Result -Prefix ($prefix + ' approve-ok')
        if ($null -ne $dialog2) {
            $dialog2Check = Test-CsApproveDialog -Dialog $dialog2
            [void](Add-CsCheck -Result $Result -Name ($prefix + ' second confirmation box is the product dialog') `
                -Pass $dialog2Check.Ok -Detail $dialog2Check.Detail)
        } else {
            $dialog2Check = $null
        }
        if ($null -ne $dialog2Check -and $dialog2Check.Ok) {
            Invoke-CsDialogButton -Dialog $dialog2.Hwnd -ButtonId 1   # IDOK
            Start-Sleep -Milliseconds 900
            $countAfterApprove = Wait-CsFetchCount -ConfigDir $configDir
            $iniApprove = Read-CsIni -Path $iniPath
            $savedProfile = ''
            $savedUrl = ''
            if ($null -ne $iniApprove) {
                $savedProfile = [string]$iniApprove['http.approved_profile']
                $savedUrl = [string]$iniApprove['http.approved_url']
            }
            $grantSaved = ($savedProfile -eq $profile -and $savedUrl -eq $url)
            [void](Add-CsCheck -Result $Result -Name ($prefix + ' approve OK saves the exact profile/URL grant') `
                -Pass $grantSaved -Detail ('profile-saved=' + ($savedProfile -eq $profile) + ' url-saved=' + ($savedUrl -eq $url)))
            $deltaApprove = $countAfterApprove - $count0
            [void](Add-CsCheck -Result $Result -Name ($prefix + ' approval fetches exactly once (single dispatch)') `
                -Pass ($deltaApprove -eq $script:MenuFetchExact) `
                -Detail ('fetch delta=' + $deltaApprove + ' expected exactly ' + $script:MenuFetchExact + ' (one menu action = one transport entry)'))
            $fetchPositive = ($deltaApprove -eq $script:MenuFetchExact)
            if ($fetchPositive) {
                Add-DeskNote -Result $Result -Note ('fetch counter positive control: 0 -> ' + $countAfterApprove + ' after the real approval (the zero-count checks use the same file and mechanism)')
            }
        } else {
            if ($null -ne $dialog2) {
                [void](Send-CsMessageTimeout -Hwnd $dialog2.Hwnd -Message 0x0010 -TimeoutMs 4000)  # WM_CLOSE
            }
            [void](Add-CsCheck -Result $Result -Name ($prefix + ' approve OK saves the exact profile/URL grant') `
                -Pass $false -Detail 'the second box was missing or not the expected product dialog')
            [void](Add-CsCheck -Result $Result -Name ($prefix + ' approval fetches exactly once (single dispatch)') `
                -Pass $false -Detail 'no approval happened')
        }
    } else {
        # Fallback so the rest of the HTTP behaviour stays verifiable: write the
        # two approved_* keys the documented user-managed way
        # (docs/m2-http-plan.md section 3 explicitly allows manual management).
        # The in-process Store only re-reads its file at startup, so the seeded
        # grant is checked after the restart below, and it is labelled as seeded
        # everywhere. The blocked approve/cancel checks stay blocked: a skipped
        # dialog is never a pass.
        Write-CsIni -Path $iniPath -Values ([ordered]@{
            'http.profile' = $profile
            'http.url' = $url
            'http.approved_profile' = $profile
            'http.approved_url' = $url
        })
        $seededGrant = $true
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' approve OK saves the exact profile/URL grant') `
            -Pass $false -Status 'blocked' -Detail 'confirmation dialog unavailable; grant seeded manually for the restart checks')
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' approval fetches exactly once (single dispatch)') `
            -Pass $false -Status 'blocked' -Detail 'no menu approval happened')
    }

    # (3) painted evidence of the fixed synthetic response, when a fetch ran.
    if ($fetchPositive) {
        $card = Update-CsCard -ConfigDir $configDir -Previous $card
        [void](Add-CsHttpRenderCheck -Run $Run -Board $board -Card $card -Result $Result `
            -Name ($prefix + ' fixed response renders HTTP chip + rows + AS OF') -Slot '1')
        $network = Test-CsNoOutbound -ProcessId $board.ProcessId
        if ($network.Available) {
            [void](Add-CsCheck -Result $Result -Name ($prefix + ' fake fetch owns no TCP connection') `
                -Pass ($network.Count -eq 0) -Detail ('connections=' + $network.Count + ' states=' + ($network.States -join ',') + ' reason=' + $network.Reason))
        } else {
            [void](Add-CsCheck -Result $Result -Name ($prefix + ' fake fetch owns no TCP connection') `
                -Pass $false -Status 'blocked' -Detail ('Get-NetTCPConnection unavailable: ' + $network.Reason))
        }

        # (4) one more manual Reload must fetch again (positive count control).
        $beforeReload = Wait-CsFetchCount -ConfigDir $configDir
        [void](Invoke-CsCardMenuItem -Board $board -Card $card -Pattern 'Reload cardspec.json*' -Result $Result -Prefix ($prefix + ' reload-manual'))
        Start-Sleep -Milliseconds $SettleMs
        $afterReload = Wait-CsFetchCount -ConfigDir $configDir
        $deltaReload = $afterReload - $beforeReload
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' manual Reload fetches exactly once') `
            -Pass ($deltaReload -eq $script:MenuFetchExact) `
            -Detail ('fetch delta=' + $deltaReload + ' (' + $beforeReload + ' -> ' + $afterReload + ') expected exactly ' + $script:MenuFetchExact))
    } else {
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' fixed response renders HTTP chip + rows + AS OF') `
            -Pass $false -Status 'blocked' -Detail 'no successful fetch in this phase; verified after the restart')
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' fake fetch owns no TCP connection') `
            -Pass $false -Status 'blocked' -Detail 'no fetch happened in this phase')
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' manual Reload fetches exactly once') `
            -Pass $false -Status 'blocked' -Detail 'no grant yet; the seeded grant is checked after the restart')
    }

    # (5) restart: grant + visibility persist, the initial Load never fetches.
    $iniGrant = Read-CsIni -Path $iniPath
    $grantPresent = ($null -ne $iniGrant -and [string]$iniGrant['http.approved_profile'] -eq $profile)
    Close-CsBoard -Run $Run -Board $board -Result $Result -Prefix $prefix
    Start-Sleep -Milliseconds 800
    $restarted = Start-CsBoard -Run $Run -ConfigDir $configDir -Suffix $Run.SuffixHttp -Tag 'http-board-restart'
    $windowRestart = Wait-CsBoardWindow -Board $restarted -TimeoutSeconds 25
    if (-not $windowRestart) {
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' restart shows the board again') -Pass $false -Detail 'no window after restart')
        Close-CsBoard -Run $Run -Board $restarted -Result $Result -Prefix ($prefix + '-restart')
        return
    }
    Start-Sleep -Milliseconds $SettleMs
    $cardRestart = Wait-CsCard -ConfigDir $configDir -TimeoutSeconds 10
    [void](Add-CsCheck -Result $Result -Name ($prefix + ' restart keeps CARDSPEC visible without Add module') `
        -Pass ($null -ne $cardRestart -and $cardRestart.Visible -eq 1) `
        -Detail $(if ($cardRestart) { 'visible=' + $cardRestart.Visible } else { 'card missing' }))
    $countRestart = Wait-CsFetchCount -ConfigDir $configDir
    $namesRestart = $null
    if ($cardRestart) { $namesRestart = Get-CsCardMenuNames -Board $restarted -Card $cardRestart }
    $grantRecognized = ($null -ne $namesRestart -and @($namesRestart | Where-Object { $_ -like 'Revoke HTTP approval*' }).Count -eq 1)
    $Result.Evidence.Add(('http restart card menu: ' + $(if ($namesRestart) { $namesRestart -join ' | ' } else { '<unavailable>' })))
    [void](Add-CsCheck -Result $Result -Name ($prefix + ' restart Load does not fetch (count 0)') `
        -Pass ($countRestart -eq 0) -Detail ('count=' + $countRestart))
    if ($null -eq $namesRestart) {
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' restart recognizes the saved grant (Revoke offered)') `
            -Pass $false -Status 'blocked' -Detail ('MSAA menu labels unavailable; grant on disk=' + $grantPresent))
    } else {
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' restart recognizes the saved grant (Revoke offered)') `
            -Pass ($grantPresent -and $grantRecognized) -Detail ('grant=' + $grantPresent + ' revoke-item=' + $grantRecognized))
    }

    # (6) with the grant, a manual Reload still fetches after the restart.
    if ($cardRestart) {
        $beforePost = Wait-CsFetchCount -ConfigDir $configDir
        [void](Invoke-CsCardMenuItem -Board $restarted -Card $cardRestart -Pattern 'Reload cardspec.json*' -Result $Result -Prefix ($prefix + ' reload-after-restart'))
        Start-Sleep -Milliseconds $SettleMs
        $afterPost = Wait-CsFetchCount -ConfigDir $configDir
        $deltaPost = $afterPost - $beforePost
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' Reload after restart fetches exactly once') `
            -Pass ($deltaPost -eq $script:MenuFetchExact) `
            -Detail ('fetch delta=' + $deltaPost + ' (' + $beforePost + ' -> ' + $afterPost + ') expected exactly ' + $script:MenuFetchExact))
        if ($seededGrant) {
            Add-DeskNote -Result $Result -Note ('fetch counter positive control (seeded grant): ' + $beforePost + ' -> ' + $afterPost + ' after a manual Reload')
            # Explicitly labelled: this grant came from the documented user-managed
            # keys, not from the menu approval path (which is reported blocked).
            [void](Add-CsHttpRenderCheck -Run $Run -Board $restarted -Card $cardRestart -Result $Result `
                -Name ($prefix + ' seeded grant renders the fixed response (labelled)') -Slot '0')
            $networkSeeded = Test-CsNoOutbound -ProcessId $restarted.ProcessId
            if ($networkSeeded.Available) {
                [void](Add-CsCheck -Result $Result -Name ($prefix + ' seeded fake fetch owns no TCP connection') `
                    -Pass ($networkSeeded.Count -eq 0) -Detail ('connections=' + $networkSeeded.Count + ' reason=' + $networkSeeded.Reason))
            } else {
                [void](Add-CsCheck -Result $Result -Name ($prefix + ' seeded fake fetch owns no TCP connection') `
                    -Pass $false -Status 'blocked' -Detail ('Get-NetTCPConnection unavailable: ' + $networkSeeded.Reason))
            }
        }

        # (7) revoke clears the grant and the old values; later refreshes never fetch.
        $countBeforeRevoke = Wait-CsFetchCount -ConfigDir $configDir
        [void](Invoke-CsCardMenuItem -Board $restarted -Card $cardRestart -Pattern 'Revoke HTTP approval*' -Result $Result -Prefix ($prefix + ' revoke'))
        Start-Sleep -Milliseconds 900
        $iniRevoked = Read-CsIni -Path $iniPath
        $grantCleared = ($null -ne $iniRevoked -and [string]::IsNullOrEmpty([string]$iniRevoked['http.approved_profile']) -and
                         [string]::IsNullOrEmpty([string]$iniRevoked['http.approved_url']))
        $countRevoked = Wait-CsFetchCount -ConfigDir $configDir
        $frameRevoked = Get-CsFrame -Run $Run -Board $restarted -Slot 0 -Label 'http-after-revoke'
        $revokedOk = $false
        $revokedDetail = 'frame unavailable'
        if ($frameRevoked.Ok) {
            $heightRevoked = Measure-CsCardHeight -Frame $frameRevoked -Card $cardRestart
            $bodyRevoked = Get-CsBodyRect -Card $cardRestart -CardHeight $(if ($heightRevoked -gt 0) { $heightRevoked } else { $script:WideMinHeight })
            $bandsRevoked = Get-CsInkBands -Frame $frameRevoked -X0 ($cardRestart.X + $script:CardPadX) -X1 ($cardRestart.X + $cardRestart.W - $script:CardPadX) `
                -Y0 $bodyRevoked.Y -Y1 ($bodyRevoked.Y + $bodyRevoked.H)
            $belowRevoked = Get-CsInkBandsBelow -Bands $bandsRevoked -Top ([int]$bodyRevoked.Y + 90)
            $chipRevoked = Get-CsChipState -Colors (Get-CsChipColors -Frame $frameRevoked -Card $cardRestart)
            $revokedOk = ($belowRevoked.Count -eq 0 -and $chipRevoked -eq 'warn')
            $revokedDetail = ('chip=' + $chipRevoked + ' below+90=' + $belowRevoked.Count)
        }
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' revoke clears the grant keys') -Pass $grantCleared -Detail ('grant-empty=' + $grantCleared))
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' revoke clears the old HTTP values') -Pass $revokedOk -Detail $revokedDetail)
        Close-CsFrame -Frame $frameRevoked
        $deltaRevoke = $countRevoked - $countBeforeRevoke
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' refresh after revoke does not fetch') `
            -Pass ($deltaRevoke -eq 0) -Detail ('fetch delta=' + $deltaRevoke + ' (' + $countBeforeRevoke + ' -> ' + $countRevoked + ')'))
        $beforeAgain = Wait-CsFetchCount -ConfigDir $configDir
        [void](Invoke-CsCardMenuItem -Board $restarted -Card $cardRestart -Pattern 'Reload cardspec.json*' -Result $Result -Prefix ($prefix + ' reload-after-revoke'))
        Start-Sleep -Milliseconds $SettleMs
        $afterAgain = Wait-CsFetchCount -ConfigDir $configDir
        [void](Add-CsCheck -Result $Result -Name ($prefix + ' Reload without a grant never fetches') `
            -Pass (($afterAgain - $beforeAgain) -eq 0 -and $afterAgain -eq $countBeforeRevoke) `
            -Detail ('count ' + $beforeAgain + ' -> ' + $afterAgain + ' (baseline ' + $countBeforeRevoke + ')'))
        $namesAfter = Get-CsCardMenuNames -Board $restarted -Card $cardRestart
        if ($null -eq $namesAfter) {
            [void](Add-CsCheck -Result $Result -Name ($prefix + ' after revoke the menu offers Approve again') `
                -Pass $false -Status 'blocked' -Detail 'MSAA menu labels unavailable')
        } else {
            $offersApproveAgain = (@($namesAfter | Where-Object { $_ -like 'Approve HTTP endpoint*' }).Count -eq 1)
            [void](Add-CsCheck -Result $Result -Name ($prefix + ' after revoke the menu offers Approve again') `
                -Pass $offersApproveAgain -Detail ($namesAfter -join ' | '))
        }
    }
    Close-CsBoard -Run $Run -Board $restarted -Result $Result -Prefix ($prefix + '-restart')
}

# ---------------------------------------------------------------------------
# leaked-process scan
# ---------------------------------------------------------------------------

function Find-CsImageProcesses {
    # Read-only system-wide metadata scan for this run's copied image. Found
    # processes are reported; they are only stopped through the recorded
    # identity of a board this script started.
    param([string]$ImagePath)
    $found = New-Object System.Collections.Generic.List[object]
    if (-not $ImagePath) { return [object[]]$found.ToArray() }
    $target = [System.IO.Path]::GetFullPath($ImagePath)
    foreach ($process in @(Get-CimInstance Win32_Process -Filter "Name='desk.exe'" -ErrorAction SilentlyContinue)) {
        $path = ''
        try { $path = [string]$process.ExecutablePath } catch { }
        if (-not $path) { continue }
        $full = ''
        try { $full = [System.IO.Path]::GetFullPath($path) } catch { continue }
        if (-not [string]::Equals($full, $target, [System.StringComparison]::OrdinalIgnoreCase)) { continue }
        $found.Add([pscustomobject]@{ ProcessId = [int]$process.ProcessId; Name = [string]$process.Name })
    }
    return [object[]]$found.ToArray()
}

# ---------------------------------------------------------------------------
# main body
# ---------------------------------------------------------------------------

$result = New-DeskResult
$script:Result = $result
$exitCode = 1

try {
    $sourceExe = $ExePath
    if (-not $sourceExe) { $sourceExe = Join-Path $script:BuildRoot 'bin\desk.exe' }
    if (-not (Test-Path -LiteralPath $sourceExe -PathType Leaf)) {
        Write-Host ('[cardspec-gui] board image missing: ' + $sourceExe) -ForegroundColor Yellow
        Write-Host '[cardspec-gui] nothing was started; build build\bin\desk.exe first.'
        exit 2
    }
    $sourceExe = [System.IO.Path]::GetFullPath($sourceExe)

    $run = New-CsRunLayout
    $script:Run = $run
    $run.SourceExe = $sourceExe
    Copy-CsBoardImage -Run $run -SourceExe $sourceExe

    Invoke-CsStaticSuite -Result $result -ScriptPath $PSCommandPath -ExePath $run.Exe

    $runInfo = [ordered]@{
        runDir      = $run.Dir
        sourceExe   = $run.SourceExe
        copiedExe   = $run.Exe
        sourceSha256 = (Get-FileHash -LiteralPath $run.SourceExe -Algorithm SHA256).Hash
        copiedSha256 = (Get-FileHash -LiteralPath $run.Exe -Algorithm SHA256).Hash
        runtimeDlls = [string[]]@($run.RuntimeDlls)
        suffixes    = [string[]]@($run.SuffixInline, $run.SuffixHttp, $run.SuffixNegA, $run.SuffixNegB)
        classes     = [string[]]@($run.ClassInline, $run.ClassHttp)
        fakeTransport = 'src/apps/desk/main.cpp cardspec-only branch (fixed native JSON, never DNS/WinHTTP)'
        gui         = 'not-run'
    }

    # Candidate provenance, asserted for static and GUI runs alike: the working
    # copy must be byte-identical to the reviewed candidate, and that image must
    # carry a signer without NotSigned/HashMismatch. Read-only checks.
    $sourceHash = [string]$runInfo.sourceSha256
    $copiedHash = [string]$runInfo.copiedSha256
    [void](Add-CsCheck -Result $result -Name 'candidate: copied image is byte-identical to the source exe' `
        -Pass ($sourceHash -eq $copiedHash -and $sourceHash.Length -eq 64) `
        -Detail ('source=' + $sourceHash + ' copied=' + $copiedHash))
    $signerPresent = $false
    $signatureText = 'unavailable'
    try {
        $signature = Get-AuthenticodeSignature -LiteralPath $run.Exe -ErrorAction Stop
        $signatureText = [string]$signature.Status
        $signerPresent = ($null -ne $signature.SignerCertificate)
    } catch {
        $signatureText = 'unavailable'
    }
    $signatureOk = ($signerPresent -and $signatureText -ne 'NotSigned' -and $signatureText -ne 'HashMismatch')
    [void](Add-CsCheck -Result $result -Name 'candidate: copied image has a signer and no NotSigned/HashMismatch' `
        -Pass $signatureOk -Detail ('status=' + $signatureText + ' signer=' + $signerPresent))

    if ($StaticOnly) {
        Add-DeskNote -Result $result -Note 'StaticOnly: AST + image hook checks only. No window, menu, dialog, fetch or network was exercised.'
        $path = Join-Path $run.EvidenceDir 'static.json'
        [void](Save-CsResult -Result $result -Path $path -Name 'cardspec-gui-static' -Extra @{ run = [pscustomobject]$runInfo })
        Write-Host ('[cardspec-gui] static result: ' + $path)
        $counts = Get-CsCounts -Result $result
        if ($counts.Fail -eq 0 -and $counts.Blocked -eq 0) { exit 0 }
        exit 1
    }

    $runInfo.gui = 'run'
    $runBefore = Get-DeskRunKeySnapshot
    $prodBefore = @(Get-CsProductionSnapshot)
    $Result.Evidence.Add(('source image sha256: ' + $runInfo.sourceSha256))
    $Result.Evidence.Add(('copied image sha256: ' + $runInfo.copiedSha256))
    $Result.Evidence.Add(('run dir: ' + $run.Dir))
    $Result.Evidence.Add(('copied image: ' + $run.Exe))
    $Result.Evidence.Add(('runtime dlls copied: ' + $(if ($run.RuntimeDlls.Count -gt 0) { $run.RuntimeDlls -join ',' } else { 'none' })))
    $Result.Evidence.Add(('isolation: instance=' + $run.SuffixInline + '/' + $run.SuffixHttp + ' class=' + $run.ClassInline + '/' + $run.ClassHttp))

    Invoke-CsNegativeSuite -Run $run -Result $result
    Invoke-CsInlineSuite -Run $run -Result $result
    Invoke-CsHttpSuite -Run $run -Result $result

    # Cleanup: every recorded board is closed, then the copied image may not
    # survive anywhere. Anything left is reported (never killed blindly). Each
    # post-suite step is independent so one failure cannot lose the report.
    try {
        foreach ($board in $script:Boards.ToArray()) {
            if (-not $board.Closed) {
                Close-CsBoard -Run $run -Board $board -Result $result -Prefix ([string]$board.Tag)
            }
        }
    } catch {
        [void](Add-CsCheck -Result $result -Name 'cleanup: recorded boards closed without an exception' -Pass $false `
            -Detail ($_.Exception.Message + ' @ line ' + $_.InvocationInfo.ScriptLineNumber))
    }
    try {
        Start-Sleep -Milliseconds 600
        $leaked = @(Find-CsImageProcesses -ImagePath $run.Exe)
        $leakedText = ''
        if ($leaked.Count -gt 0) {
            $leakedText = (@($leaked | ForEach-Object { '{0}:{1}' -f $_.ProcessId, $_.Name }) -join ',')
        }
        [void](Add-CsCheck -Result $result -Name 'cleanup: no board process of this run survives' `
            -Pass ($leaked.Count -eq 0) -Detail $leakedText)
    } catch {
        [void](Add-CsCheck -Result $result -Name 'cleanup: no board process of this run survives' -Pass $false `
            -Detail ('scan failed: ' + $_.Exception.Message))
    }
    try {
        $runAfter = Get-DeskRunKeySnapshot
        $runChanges = @(Compare-DeskRunKey -Before $runBefore -After $runAfter)
        [void](Add-CsCheck -Result $result -Name 'isolation: HKCU Run is unchanged' `
            -Pass ($runChanges.Count -eq 0) -Detail ($runChanges -join '; '))
    } catch {
        [void](Add-CsCheck -Result $result -Name 'isolation: HKCU Run is unchanged' -Pass $false `
            -Detail ('comparison failed: ' + $_.Exception.Message))
    }
    try {
        $prodAfter = @(Get-CsProductionSnapshot)
        Add-CsProductionChecks -Result $result -Before $prodBefore -After $prodAfter
    } catch {
        [void](Add-CsCheck -Result $result -Name 'isolation: production config metadata is unchanged' -Pass $false `
            -Detail ('fingerprint comparison failed: ' + $_.Exception.Message))
    }

    Add-DeskNote -Result $result -Note ('fake transport only: every fetch was the fixed native fixture; this run is NOT a TLS/HTTP acceptance')
    $path = Join-Path $run.EvidenceDir 'gui.json'
    [void](Save-CsResult -Result $result -Path $path -Extra @{ run = [pscustomobject]$runInfo })
    Write-Host ('[cardspec-gui] result: ' + $path)
} catch {
    $failure = $_
    [void](Add-CsCheck -Result $result -Name 'suite completed without an exception' -Pass $false `
        -Detail ($failure.Exception.Message + ' @ line ' + $failure.InvocationInfo.ScriptLineNumber))
    Write-Host ('[cardspec-gui] exception: ' + $failure.Exception.Message) -ForegroundColor Yellow
    # The abort path must still leave a report, and nothing in it may throw.
    try {
        if ($null -ne $script:Run) {
            foreach ($board in $script:Boards.ToArray()) {
                if (-not $board.Closed) {
                    try { Close-CsBoard -Run $script:Run -Board $board -Result $result -Prefix ([string]$board.Tag) } catch { }
                }
            }
            $path = Join-Path $script:Run.EvidenceDir 'gui.json'
            [void](Save-CsResult -Result $result -Path $path -Extra @{ run = [pscustomobject]@{ runDir = $script:Run.Dir; copiedExe = $script:Run.Exe; gui = 'aborted' } })
            Write-Host ('[cardspec-gui] aborted result: ' + $path)
        }
    } catch {
        Write-Host ('[cardspec-gui] abort reporting failed: ' + $_.Exception.Message) -ForegroundColor Red
    }
} finally {
    $counts = Get-CsCounts -Result $result
    Write-Host ('[cardspec-gui] checks: pass={0} fail={1} blocked={2} total={3}' -f $counts.Pass, $counts.Fail, $counts.Blocked, $counts.Total)
    if ($counts.Fail -eq 0 -and $counts.Blocked -eq 0 -and $counts.Total -gt 0) { $exitCode = 0 }
    if ($counts.Blocked -gt 0) {
        Write-Host '[cardspec-gui] blocked checks are NOT passes: see the result JSON.' -ForegroundColor Yellow
    }
    Write-Host '[cardspec-gui] done (only PIDs started by this script were stopped).'
}

exit $exitCode
