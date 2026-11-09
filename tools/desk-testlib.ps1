# desk-testlib.ps1 - shared Win32 helpers for the desk QA scripts.
#
# Loaded (dot-sourced) by desk-testrun.ps1, desk-ui-test.ps1 and
# desk-perf.ps1. Everything here is read-only towards the live installed app:
# the only processes these helpers start, message or stop are the ones a test
# script launched itself under build\testrun.
#
# Conventions:
#   * board coordinates are window-client pixels,
#   * card coordinates are interior-relative (desk.ini card.<id>.x/y),
#   * all functions are safe to call with a missing window/process and return
#     $null / empty instead of throwing where a test has to observe that state.

if (-not ('DeskTestNative' -as [type])) {
    Add-Type -AssemblyName System.Drawing

    $source = @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;

public static class DeskTestNative {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int X, Y; }

    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr hWnd, StringBuilder text, int count);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    // WM_CHAR is thunked by user32 even though wParam is an integer: use W explicitly.
    [DllImport("user32.dll", EntryPoint = "SendMessageW", ExactSpelling = true, CharSet = CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll", EntryPoint = "PostMessageW", ExactSpelling = true, CharSet = CharSet.Unicode)] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SystemParametersInfo(uint action, uint param, out RECT rect, uint winIni);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr hWnd);
    [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
    [DllImport("user32.dll")] public static extern uint GetGuiResources(IntPtr process, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT point);

    public const int WM_CLOSE = 0x0010;
    public const int WM_LBUTTONDOWN = 0x0201;
    public const int WM_LBUTTONUP = 0x0202;
    public const int WM_MOUSEMOVE = 0x0200;
    public const int WM_MOUSEWHEEL = 0x020A;
    public const int WM_CHAR = 0x0102;
    public const int WM_KEYDOWN = 0x0100;
    public const int WM_KEYUP = 0x0101;

    public static IntPtr MakeLParam(int x, int y) {
        return (IntPtr)(((y & 0xFFFF) << 16) | (x & 0xFFFF));
    }

    public static bool PostClick(IntPtr hwnd, int x, int y) {
        bool ok = PostMessage(hwnd, WM_LBUTTONDOWN, (IntPtr)1, MakeLParam(x, y));
        ok &= PostMessage(hwnd, WM_LBUTTONUP, IntPtr.Zero, MakeLParam(x, y));
        return ok;
    }

    public static bool PostDrag(IntPtr hwnd, int x0, int y0, int x1, int y1, int steps) {
        if (steps < 1) steps = 1;
        bool ok = PostMessage(hwnd, WM_LBUTTONDOWN, (IntPtr)1, MakeLParam(x0, y0));
        for (int i = 1; i <= steps; ++i) {
            int x = x0 + (x1 - x0) * i / steps;
            int y = y0 + (y1 - y0) * i / steps;
            ok &= PostMessage(hwnd, WM_MOUSEMOVE, (IntPtr)1, MakeLParam(x, y));
        }
        ok &= PostMessage(hwnd, WM_LBUTTONUP, IntPtr.Zero, MakeLParam(x1, y1));
        return ok;
    }

    // WM_MOUSEWHEEL carries screen coordinates, not client coordinates.
    public static bool PostWheel(IntPtr hwnd, int screenX, int screenY, int delta) {
        int wp = ((delta & 0xFFFF) << 16);
        return PostMessage(hwnd, WM_MOUSEWHEEL, (IntPtr)wp, MakeLParam(screenX, screenY));
    }

    public static bool PostChar(IntPtr hwnd, int ch) {
        return PostMessage(hwnd, WM_CHAR, (IntPtr)ch, (IntPtr)1);
    }

    public static bool PostKey(IntPtr hwnd, int vk) {
        return PostMessage(hwnd, WM_KEYDOWN, (IntPtr)vk, (IntPtr)1);
    }
}

public static class DeskTestImage {
    public static Bitmap ScreenBitmap(Rectangle r) {
        if (r.Width <= 0 || r.Height <= 0) return null;
        Bitmap bmp = new Bitmap(r.Width, r.Height, PixelFormat.Format32bppArgb);
        using (Graphics g = Graphics.FromImage(bmp)) {
            g.CopyFromScreen(r.Left, r.Top, 0, 0, r.Size, CopyPixelOperation.SourceCopy);
        }
        return bmp;
    }

    public static Bitmap PrintBitmap(IntPtr hwnd, int width, int height) {
        if (width <= 0 || height <= 0) return null;
        Bitmap bmp = new Bitmap(width, height, PixelFormat.Format32bppArgb);
        using (Graphics g = Graphics.FromImage(bmp)) {
            IntPtr dc = g.GetHdc();
            try { DeskTestNative.PrintWindow(hwnd, dc, 2u); } finally { g.ReleaseHdc(dc); }
        }
        return bmp;
    }

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

    static int Lum(byte[] b, int index) {
        return (b[index + 2] * 30 + b[index + 1] * 59 + b[index] * 11) / 100;
    }

    // "colors" counts distinct RGB values on a coarse grid; "alpha" is the
    // fraction of sampled pixels with a non-zero alpha channel. A layered
    // window printed through DefWindowProc typically comes back fully
    // transparent/black, which is what makes the screen-capture fallback
    // necessary and detectable.
    public static string Stats(Bitmap bmp) {
        if (bmp == null) return "null";
        int stride, width, height;
        byte[] bytes = Lock(bmp, out stride, out width, out height);
        HashSet<int> colors = new HashSet<int>();
        long alphaCount = 0;
        long sum = 0;
        long sum2 = 0;
        long n = 0;
        for (int y = 0; y < height; y += 2) {
            int row = y * stride;
            for (int x = 0; x < width; x += 2) {
                int i = row + x * 4;
                int a = bytes[i + 3];
                if (a != 0) ++alphaCount;
                int lum = Lum(bytes, i);
                sum += lum; sum2 += (long)lum * lum; ++n;
                if (colors.Count < 4000) colors.Add((bytes[i + 2] << 16) | (bytes[i + 1] << 8) | bytes[i]);
            }
        }
        double mean = n == 0 ? 0 : (double)sum / n;
        double sd = n == 0 ? 0 : Math.Sqrt(Math.Max(0.0, (double)sum2 / n - mean * mean));
        double alpha = n == 0 ? 0 : (double)alphaCount / n;
        return string.Format("colors={0} alpha={1:F3} mean={2:F1} sd={3:F1}",
            colors.Count, alpha, mean, sd);
    }

    public static bool IsBlank(Bitmap bmp) {
        if (bmp == null) return true;
        int stride, width, height;
        byte[] bytes = Lock(bmp, out stride, out width, out height);
        HashSet<int> colors = new HashSet<int>();
        long sum = 0, sum2 = 0, n = 0;
        for (int y = 0; y < height; y += 3) {
            int row = y * stride;
            for (int x = 0; x < width; x += 3) {
                int i = row + x * 4;
                colors.Add((bytes[i + 2] << 16) | (bytes[i + 1] << 8) | bytes[i]);
                int lum = Lum(bytes, i);
                sum += lum; sum2 += (long)lum * lum; ++n;
            }
        }
        double mean = n == 0 ? 0 : (double)sum / n;
        double sd = n == 0 ? 0 : Math.Sqrt(Math.Max(0.0, (double)sum2 / n - mean * mean));
        // A layered window printed by DefWindowProc comes back (near) uniformly
        // black; a real board has chrome, cards and text (sd well above 5).
        return colors.Count <= 24 && sd < 5.0;
    }

    // Longest run of consecutive dark pixels on one row (console interior).
    public static int LongestDarkRun(Bitmap bmp, int y, int x0, int x1, int maxLum) {
        if (bmp == null) return 0;
        int stride, width, height;
        byte[] bytes = Lock(bmp, out stride, out width, out height);
        if (y < 0 || y >= height) return 0;
        x0 = Math.Max(0, x0); x1 = Math.Min(width, x1);
        int best = 0, run = 0;
        int row = y * stride;
        for (int x = x0; x < x1; ++x) {
            int i = row + x * 4;
            if (Lum(bytes, i) <= maxLum) { ++run; if (run > best) best = run; }
            else run = 0;
        }
        return best;
    }

    // Runs of columns that contain ink (luminance >= minLum) in rows [y0, y1).
    // A 5x7 ASCII glyph stays inside one 7 px cell; a 14 px CJK glyph spans
    // two cells, so the run widths are the evidence for "wide = two cells".
    public static int[] InkRuns(Bitmap bmp, int y0, int y1, int x0, int x1, int minLum) {
        if (bmp == null) return new int[0];
        int stride, width, height;
        byte[] bytes = Lock(bmp, out stride, out width, out height);
        y0 = Math.Max(0, y0); y1 = Math.Min(height, y1);
        x0 = Math.Max(0, x0); x1 = Math.Min(width, x1);
        List<int> runs = new List<int>();
        int run = 0;
        for (int x = x0; x < x1; ++x) {
            bool ink = false;
            for (int y = y0; y < y1; ++y) {
                if (Lum(bytes, y * stride + x * 4) >= minLum) { ink = true; break; }
            }
            if (ink) { ++run; }
            else if (run > 0) { runs.Add(run); run = 0; }
        }
        if (run > 0) runs.Add(run);
        return runs.ToArray();
    }

    public static int CountInkPixels(Bitmap bmp, int y0, int y1, int x0, int x1, int minLum) {
        if (bmp == null) return 0;
        int stride, width, height;
        byte[] bytes = Lock(bmp, out stride, out width, out height);
        y0 = Math.Max(0, y0); y1 = Math.Min(height, y1);
        x0 = Math.Max(0, x0); x1 = Math.Min(width, x1);
        int count = 0;
        for (int y = y0; y < y1; ++y) {
            int row = y * stride;
            for (int x = x0; x < x1; ++x) {
                if (Lum(bytes, row + x * 4) >= minLum) ++count;
            }
        }
        return count;
    }

    // Row indices in [y0, y1) that contain at least one ink pixel.
    public static int[] InkRows(Bitmap bmp, int x0, int x1, int y0, int y1, int minLum) {
        if (bmp == null) return new int[0];
        int stride, width, height;
        byte[] bytes = Lock(bmp, out stride, out width, out height);
        x0 = Math.Max(0, x0); x1 = Math.Min(width, x1);
        y0 = Math.Max(0, y0); y1 = Math.Min(height, y1);
        List<int> rows = new List<int>();
        for (int y = y0; y < y1; ++y) {
            int row = y * stride;
            bool ink = false;
            for (int x = x0; x < x1; ++x) {
                if (Lum(bytes, row + x * 4) >= minLum) { ink = true; break; }
            }
            if (ink) rows.Add(y);
        }
        return rows.ToArray();
    }

    // Width from the leftmost to the rightmost ink column (0 when empty).
    public static int InkSpan(Bitmap bmp, int y0, int y1, int x0, int x1, int minLum) {
        if (bmp == null) return 0;
        int stride, width, height;
        byte[] bytes = Lock(bmp, out stride, out width, out height);
        y0 = Math.Max(0, y0); y1 = Math.Min(y1, height);
        x0 = Math.Max(0, x0); x1 = Math.Min(width, x1);
        int first = -1, last = -1;
        for (int x = x0; x < x1; ++x) {
            bool ink = false;
            for (int y = y0; y < y1; ++y) {
                if (Lum(bytes, y * stride + x * 4) >= minLum) { ink = true; break; }
            }
            if (ink) { if (first < 0) first = x; last = x; }
        }
        return first < 0 ? 0 : last - first + 1;
    }

    // Count pixels whose colour is dominated by red+green (the warning yellow
    // used for the scrolled-back status chip and the BOTTOM button).
    public static int CountWarmPixels(Bitmap bmp, int x0, int y0, int w, int h) {
        if (bmp == null) return 0;
        int stride, width, height;
        byte[] bytes = Lock(bmp, out stride, out width, out height);
        x0 = Math.Max(0, x0); y0 = Math.Max(0, y0);
        w = Math.Min(w, width - x0); h = Math.Min(h, height - y0);
        int count = 0;
        for (int y = y0; y < y0 + h; ++y) {
            int row = y * stride;
            for (int x = x0; x < x0 + w; ++x) {
                int i = row + x * 4;
                int b = bytes[i], g = bytes[i + 1], r = bytes[i + 2];
                if (r >= 150 && g >= 110 && b <= 110 && (r - b) >= 60) ++count;
            }
        }
        return count;
    }

    public static double DiffFraction(Bitmap a, Bitmap b, int x0, int y0, int w, int h, int threshold) {
        if (a == null || b == null) return 1.0;
        int strideA, wA, hA, strideB, wB, hB;
        byte[] ba = Lock(a, out strideA, out wA, out hA);
        byte[] bb = Lock(b, out strideB, out wB, out hB);
        x0 = Math.Max(0, x0); y0 = Math.Max(0, y0);
        w = Math.Min(w, Math.Min(wA, wB) - x0);
        h = Math.Min(h, Math.Min(hA, hB) - y0);
        if (w <= 0 || h <= 0) return 0.0;
        long diff = 0, total = 0;
        for (int y = y0; y < y0 + h; ++y) {
            int ra = y * strideA, rb = y * strideB;
            for (int x = x0; x < x0 + w; ++x) {
                int ia = ra + x * 4, ib = rb + x * 4;
                int d = Math.Abs(Lum(ba, ia) - Lum(bb, ib));
                if (d >= threshold) ++diff;
                ++total;
            }
        }
        return total == 0 ? 0.0 : (double)diff / total;
    }
}

// Launches the test copy without inheriting this process' console.
//
// A desktop app is normally started by Explorer, i.e. with no console. When a
// GUI process is started from a console process, it inherits that console, and
// the ConPTY client (pwsh) inside the POWERSHELL card then attaches to it
// instead of the pseudo console - the card shows EXITED. DETACHED_PROCESS keeps
// the test instance equivalent to the real deployment.
public static class DeskLauncher {
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

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool CreateProcess(string lpApplicationName, string lpCommandLine,
        IntPtr lpProcessAttributes, IntPtr lpThreadAttributes, bool bInheritHandles,
        uint dwCreationFlags, IntPtr lpEnvironment, string lpCurrentDirectory,
        ref STARTUPINFO si, out PROCESS_INFORMATION pi);

    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);

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

    public static int StartDetached(string exe, string workingDirectory, string envName, string envValue) {
        return StartDetachedEx(exe, workingDirectory, new string[] { envName }, new string[] { envValue });
    }

    // Extra environment variables (DESK_TEST_INSTANCE + DESK_TEST_DISPLAY_FILE for
    // the display/DPI simulation scenarios).
    public static int StartDetachedEx(string exe, string workingDirectory, string[] names, string[] values) {
        STARTUPINFO si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
        PROCESS_INFORMATION pi = new PROCESS_INFORMATION();
        // DETACHED_PROCESS: no inherited console. CREATE_UNICODE_ENVIRONMENT:
        // the explicit environment block is UTF-16.
        const uint flags = 0x00000008u | 0x00000400u | 0x08000000u;
        IntPtr env = BuildEnvironment(names, values);
        try {
            string commandLine = "\"" + exe + "\"";
            if (!CreateProcess(exe, commandLine, IntPtr.Zero, IntPtr.Zero, false, flags, env,
                               workingDirectory, ref si, out pi)) {
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            }
        } finally {
            Marshal.FreeHGlobal(env);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return pi.dwProcessId;
    }
}
// MSAA: the only reliable way to activate an item in another process' Win32
// popup menu (the popup does not appear in the UIA tree and does not take
// posted WM_KEYDOWN). Used by the QA reset-panel scenario.
public static class MsaaNative {
    [DllImport("oleacc.dll")]
    public static extern int AccessibleObjectFromWindow(IntPtr hwnd, uint id,
        ref Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object obj);
}
'@

    Add-Type -ReferencedAssemblies 'System.Drawing' -TypeDefinition $source -ErrorAction Stop
}

# ---------------------------------------------------------------------------
# process / window discovery
# ---------------------------------------------------------------------------

function Get-DeskWindowList {
    param([int]$ProcessId = 0)

    $windows = New-Object System.Collections.Generic.List[object]
    $callback = [DeskTestNative+EnumWindowsProc]{
        param([IntPtr]$hwnd, [IntPtr]$lparam)
        $pidValue = [uint32]0
        [void][DeskTestNative]::GetWindowThreadProcessId($hwnd, [ref]$pidValue)
        if ($ProcessId -ne 0 -and [int]$pidValue -ne $ProcessId) { return $true }
        $class = New-Object System.Text.StringBuilder 256
        [void][DeskTestNative]::GetClassName($hwnd, $class, $class.Capacity)
        $title = New-Object System.Text.StringBuilder 512
        [void][DeskTestNative]::GetWindowText($hwnd, $title, $title.Capacity)
        $rect = New-Object DeskTestNative+RECT
        [void][DeskTestNative]::GetWindowRect($hwnd, [ref]$rect)
        $windows.Add([pscustomobject]@{
            Hwnd      = $hwnd
            ProcessId = [int]$pidValue
            Class     = $class.ToString()
            Title     = $title.ToString()
            Visible   = [DeskTestNative]::IsWindowVisible($hwnd)
            X         = $rect.Left
            Y         = $rect.Top
            W         = $rect.Right - $rect.Left
            H         = $rect.Bottom - $rect.Top
            Right     = $rect.Right
            Bottom    = $rect.Bottom
        })
        return $true
    }
    [void][DeskTestNative]::EnumWindows($callback, [IntPtr]::Zero)
    return $windows
}

function Wait-DeskWindow {
    param(
        [int]$ProcessId,
        [string]$ClassName,
        [int]$TimeoutSeconds = 20,
        [switch]$IncludeDialogs
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $last = @()
    while ((Get-Date) -lt $deadline) {
        $last = @(Get-DeskWindowList -ProcessId $ProcessId | Where-Object {
            $_.Visible -and ($_.W -gt 50) -and
            (($ClassName -eq '') -or ($_.Class -eq $ClassName) -or
             ($IncludeDialogs -and $_.Class -eq '#32770'))
        })
        if ($last.Count -gt 0) { return $last }
        Start-Sleep -Milliseconds 100
    }
    return $last
}

function Get-DeskDialogList {
    param([int]$ProcessId)
    return @(Get-DeskWindowList -ProcessId $ProcessId |
        Where-Object { $_.Class -eq '#32770' -and $_.Visible })
}

function Get-DeskWorkArea {
    $rect = New-Object DeskTestNative+RECT
    $ok = [DeskTestNative]::SystemParametersInfo(0x0030, 0, [ref]$rect, 0)
    if (-not $ok) {
        throw 'SPI_GETWORKAREA failed'
    }
    return [pscustomobject]@{
        X = $rect.Left; Y = $rect.Top
        W = $rect.Right - $rect.Left
        H = $rect.Bottom - $rect.Top
        Right = $rect.Right; Bottom = $rect.Bottom
    }
}

# ---------------------------------------------------------------------------
# screenshots
# ---------------------------------------------------------------------------

function Save-DeskWindowShot {
    param(
        [IntPtr]$Hwnd,
        [int]$X, [int]$Y, [int]$W, [int]$H,
        [string]$Path,
        [ValidateSet('auto', 'print', 'screen')][string]$Mode = 'auto'
    )
    $bitmap = $null
    $used = ''
    if ($Mode -ne 'screen') {
        $bitmap = [DeskTestImage]::PrintBitmap($Hwnd, $W, $H)
        $used = 'PrintWindow'
        if ($Mode -eq 'auto' -and [DeskTestImage]::IsBlank($bitmap)) {
            $bitmap.Dispose()
            $bitmap = $null
        }
    }
    if (-not $bitmap) {
        $rect = New-Object System.Drawing.Rectangle($X, $Y, $W, $H)
        $bitmap = [DeskTestImage]::ScreenBitmap($rect)
        $used = 'BitBlt/screen'
    }
    if (-not $bitmap) { throw "cannot capture window $Hwnd" }
    try {
        $dir = Split-Path -Parent $Path
        if ($dir -and -not (Test-Path -LiteralPath $dir)) {
            New-Item -ItemType Directory -Path $dir -Force | Out-Null
        }
        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
        $stats = [DeskTestImage]::Stats($bitmap)
    } finally {
        $bitmap.Dispose()
    }
    return [pscustomobject]@{ Path = $Path; Mode = $used; Stats = $stats }
}

# ---------------------------------------------------------------------------
# desk.ini (UTF-16LE with BOM, key=value, same shape as wg::Store)
# ---------------------------------------------------------------------------

function Read-DeskIniFile {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $text = [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::Unicode)
    if ($text.Length -gt 0 -and $text[0] -eq [char]0xFEFF) { $text = $text.Substring(1) }
    $values = [ordered]@{}
    foreach ($line in ($text -split ([string][char]10))) {
        $line = $line.TrimEnd([char]13)
        $eq = $line.IndexOf('=')
        if ($eq -gt 0) { $values[$line.Substring(0, $eq)] = $line.Substring($eq + 1) }
    }
    return $values
}

function Write-DeskIniFile {
    param([string]$Path, [object]$Values)
    $dir = Split-Path -Parent $Path
    if ($dir -and -not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.Append([char]0xFEFF)
    if ($Values -is [System.Collections.IDictionary]) {
        foreach ($key in $Values.Keys) { [void]$sb.Append("$key=$($Values[$key])" + [char]10) }
    } else {
        foreach ($pair in $Values) { [void]$sb.Append("$($pair[0])=$($pair[1])" + [char]10) }
    }
    [System.IO.File]::WriteAllText($Path, $sb.ToString(), [System.Text.Encoding]::Unicode)
}

function Wait-IniStable {
    # True once the file exists and its size/mtime stopped changing.
    param([string]$Path, [int]$TimeoutSeconds = 6)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $last = ''
    while ((Get-Date) -lt $deadline) {
        if (Test-Path -LiteralPath $Path) {
            $item = Get-Item -LiteralPath $Path
            $fingerprint = '{0}|{1}' -f $item.LastWriteTimeUtc.Ticks, $item.Length
            if ($fingerprint -eq $last) { return $true }
            $last = $fingerprint
        }
        Start-Sleep -Milliseconds 250
    }
    return (Test-Path -LiteralPath $Path)
}

function Get-DeskIniInt {
    param([object]$Values, [string]$Key, [int]$Default = 0)
    if ($null -eq $Values -or -not $Values.Contains($Key)) { return $Default }
    $parsed = 0
    if ([int]::TryParse($Values[$Key], [ref]$parsed)) { return $parsed }
    return $Default
}

# ---------------------------------------------------------------------------
# process stats
# ---------------------------------------------------------------------------

function Get-DeskProcessStats {
    param([int]$ProcessId)
    $proc = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if (-not $proc) { return $null }
    $gdi = $null
    $user = $null
    $handle = [DeskTestNative]::OpenProcess(0x0400, $false, [uint32]$ProcessId)
    if ($handle -ne [IntPtr]::Zero) {
        try {
            $gdi = [DeskTestNative]::GetGuiResources($handle, 0)
            $user = [DeskTestNative]::GetGuiResources($handle, 1)
        } finally {
            [void][DeskTestNative]::CloseHandle($handle)
        }
    }
    return [pscustomobject]@{
        ProcessId    = $ProcessId
        WSMB         = [math]::Round($proc.WorkingSet64 / 1MB, 1)
        PrivateMB    = [math]::Round($proc.PrivateMemorySize64 / 1MB, 1)
        Threads      = $proc.Threads.Count
        GDI          = $gdi
        USER         = $user
        CpuMs        = [math]::Round($proc.TotalProcessorTime.TotalMilliseconds, 1)
        Handles      = $proc.HandleCount
    }
}

function Measure-DeskCpu {
    param([int]$ProcessId, [int]$Seconds = 10)
    $proc = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if (-not $proc) { return $null }
    $proc.Refresh()
    $cpu0 = $proc.TotalProcessorTime.TotalMilliseconds
    Start-Sleep -Seconds $Seconds
    $proc.Refresh()
    $cpu1 = $proc.TotalProcessorTime.TotalMilliseconds
    return [pscustomobject]@{
        Seconds = $Seconds
        CpuMs   = [math]::Round($cpu1 - $cpu0, 1)
        CpuPct  = [math]::Round(100.0 * ($cpu1 - $cpu0) / ($Seconds * 1000.0), 2)
    }
}

# ---------------------------------------------------------------------------
# input: posted messages only, never real cursor/keyboard injection
# ---------------------------------------------------------------------------

function Send-DeskClick {
    param([IntPtr]$Hwnd, [int]$X, [int]$Y)
    return [DeskTestNative]::PostClick($Hwnd, $X, $Y)
}

function Send-DeskDrag {
    param([IntPtr]$Hwnd, [int]$X0, [int]$Y0, [int]$X1, [int]$Y1, [int]$Steps = 30)
    return [DeskTestNative]::PostDrag($Hwnd, $X0, $Y0, $X1, $Y1, $Steps)
}

function Send-DeskWheel {
    param([IntPtr]$Hwnd, [int]$ScreenX, [int]$ScreenY, [int]$Delta)
    return [DeskTestNative]::PostWheel($Hwnd, $ScreenX, $ScreenY, $Delta)
}

function Send-DeskText {
    param([IntPtr]$Hwnd, [string]$Text)
    foreach ($ch in $Text.ToCharArray()) {
        [void][DeskTestNative]::PostChar($Hwnd, [int]$ch)
        Start-Sleep -Milliseconds 15
    }
}

function Send-DeskEnter {
    param([IntPtr]$Hwnd)
    [void][DeskTestNative]::PostKey($Hwnd, 0x0D)   # VK_RETURN
}

# ---------------------------------------------------------------------------
# test instance lifecycle (used by desk-ui-test / desk-perf)
# ---------------------------------------------------------------------------

function New-DeskTestDir {
    param([string]$Path, [string]$Root)
    $full = [System.IO.Path]::GetFullPath($Path)
    $rootFull = [System.IO.Path]::GetFullPath($Root)
    if (-not $full.StartsWith($rootFull, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "refusing to clean '$full' (outside $rootFull)"
    }
    if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction Stop }
    New-Item -ItemType Directory -Path $full -Force | Out-Null
    return $full
}

function Start-DeskTestInstance {
    param([string]$ExePath, [string]$WorkingDirectory, [string]$Suffix, [string]$DisplayFile = '')
    if ($DisplayFile) {
        return [DeskLauncher]::StartDetachedEx($ExePath, $WorkingDirectory,
            [string[]]@('DESK_TEST_INSTANCE', 'DESK_TEST_DISPLAY_FILE'),
            [string[]]@($Suffix, $DisplayFile))
    }
    return [DeskLauncher]::StartDetached($ExePath, $WorkingDirectory, 'DESK_TEST_INSTANCE', $Suffix)
}

function Write-DeskDisplayFile {
    # Test display file for the DESK_TEST_DISPLAY_FILE hook:
    #   work=x,y,w,h   (required)
    #   dpi=N
    #   rect=x,y,w,h   (optional suggested DPI rectangle)
    param([string]$Path, [string]$Work, [int]$Dpi = 96, [string]$Rect = '')
    $lines = @("work=$Work", "dpi=$Dpi")
    if ($Rect) { $lines += "rect=$Rect" }
    $directory = Split-Path -Parent $Path
    if ($directory -and -not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    [System.IO.File]::WriteAllText($Path, (($lines -join ([string][char]10)) + [string][char]10),
        (New-Object System.Text.UTF8Encoding($false)))
}

function Send-DeskDisplayMessage {
    # 0x8009 = work-area adaptation path, 0x800A = DPI-changed path.
    param([IntPtr]$Hwnd, [uint32]$Message)
    return [DeskTestNative]::PostMessage($Hwnd, $Message, [IntPtr]::Zero, [IntPtr]::Zero)
}

function Wait-DeskIniCondition {
    param([string]$Path, [scriptblock]$Predicate, [int]$TimeoutSeconds = 8)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $latest = $null
    while ((Get-Date) -lt $deadline) {
        $latest = Read-DeskIniFile -Path $Path
        if ($latest -and (& $Predicate $latest)) { return $latest }
        Start-Sleep -Milliseconds 200
    }
    return $latest
}

function Test-DeskTestAlive {
    param([int]$ProcessId)
    return [bool](Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)
}

function Wait-DeskTestExit {
    param([int]$ProcessId, [int]$TimeoutMs)
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline) {
        if (-not (Test-DeskTestAlive -ProcessId $ProcessId)) { return $true }
        Start-Sleep -Milliseconds 100
    }
    return -not (Test-DeskTestAlive -ProcessId $ProcessId)
}

function Get-DeskTestPath {
    param([int]$ProcessId)
    $process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if ($process -and $process.Path) { return $process.Path }
    return ''
}

function Stop-DeskTestInstance {
    param([int]$ProcessId, [string]$Root)
    $path = Get-DeskTestPath -ProcessId $ProcessId
    if (-not $path) { return $false }
    $full = [System.IO.Path]::GetFullPath($path)
    $rootFull = [System.IO.Path]::GetFullPath($Root)
    if (-not $full.StartsWith($rootFull, [System.StringComparison]::OrdinalIgnoreCase)) { return $false }
    Stop-Process -Id $ProcessId -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 400
    return $true
}

function Stop-AllDeskTestProcesses {
    param([string]$Root)
    $rootFull = [System.IO.Path]::GetFullPath($Root)
    $stopped = @()
    foreach ($process in @(Get-Process -Name desk -ErrorAction SilentlyContinue)) {
        $path = ''
        if ($process.Path) { $path = [System.IO.Path]::GetFullPath($process.Path) }
        if ($path -and $path.StartsWith($rootFull, [System.StringComparison]::OrdinalIgnoreCase)) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            $stopped += $process.Id
        }
    }
    if ($stopped.Count -gt 0) { Start-Sleep -Milliseconds 400 }
    return $stopped
}

function Close-DeskBoard {
    # Graceful WM_CLOSE to the board window; Stop-Process only for this PID
    # after checking the image still lives under the test root.
    param([int]$ProcessId, [IntPtr]$Hwnd, [string]$Root, [int]$TimeoutMs = 10000)
    if ($Hwnd -ne [IntPtr]::Zero) {
        [void][DeskTestNative]::PostMessage($Hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    }
    if (Wait-DeskTestExit -ProcessId $ProcessId -TimeoutMs $TimeoutMs) {
        return [pscustomobject]@{ Graceful = $true; Killed = $false }
    }
    $killed = Stop-DeskTestInstance -ProcessId $ProcessId -Root $Root
    return [pscustomobject]@{ Graceful = $false; Killed = $killed }
}

# ---------------------------------------------------------------------------
# registry guard: the harness must never change autostart values
# ---------------------------------------------------------------------------

function Get-DeskRunKeySnapshot {
    $key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
    $values = [ordered]@{}
    if (Test-Path -LiteralPath $key) {
        $item = Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue
        if ($item) {
            foreach ($property in $item.PSObject.Properties) {
                if ($property.Name -like 'PS*') { continue }
                $values[$property.Name] = [string]$property.Value
            }
        }
    }
    return $values
}

function Compare-DeskRunKey {
    param([object]$Before, [object]$After)
    $changes = @()
    foreach ($name in $Before.Keys) {
        if (-not $After.Contains($name)) {
            $changes += "removed: $name"
        } elseif ($After[$name] -ne $Before[$name]) {
            $changes += "changed: $name = '$($After[$name])' (was '$($Before[$name])')"
        }
    }
    foreach ($name in $After.Keys) {
        if (-not $Before.Contains($name)) { $changes += "added: $name = '$($After[$name])'" }
    }
    return $changes
}

# ---------------------------------------------------------------------------
# board / card geometry (mirrors src/widget/skin.h + board.cpp InteriorFor)
# ---------------------------------------------------------------------------

$script:DeskGeometry = @{
    BoardPad      = 14
    BoardTitleH   = 26
    BoardResize   = 16
    CardHeaderH   = 30
    CardPadTop    = 8
    CardPadBottom = 10
    CellW         = 7
    CellH         = 14
    ConsolePad    = 4
    Cols          = 80
    RowsCollapsed = 8
    RowsExpanded  = 24
}

function Get-DeskInterior {
    # Screen-space interior for a board window rect.
    param([int]$BoardX, [int]$BoardY, [int]$BoardW, [int]$BoardH)
    $g = $script:DeskGeometry
    return [pscustomobject]@{
        X = $BoardX + $g.BoardPad
        Y = $BoardY + $g.BoardTitleH + 6
        W = $BoardW - 2 * $g.BoardPad
        H = $BoardH - $g.BoardTitleH - 6 - $g.BoardPad - $g.BoardResize
    }
}

function Get-DeskCardRect {
    # desk.ini card.<id>.x/y are board-canvas coordinates (the interior rect is
    # only the allowed bounds, see board.cpp PlanLayout/InteriorFor), so the
    # on-screen rect is board origin + stored position.
    param(
        [int]$BoardX, [int]$BoardY,
        [int]$CardX, [int]$CardY,
        [int]$CardW, [int]$CardH
    )
    return [pscustomobject]@{
        X = $BoardX + $CardX
        Y = $BoardY + $CardY
        W = $CardW
        H = $CardH
        Right = $BoardX + $CardX + $CardW
        Bottom = $BoardY + $CardY + $CardH
    }
}

function Get-DeskCardHeight {
    # Outer card height for the default test content (no aimeter.ini/server.ini
    # in a testrun: Account has its fixed 3 rows, Servers one placeholder row,
    # the terminal has fixed 8/24 rows). Mirrors docsdesk.md section 4.
    param([string]$Id, [bool]$Expanded)
    switch ($Id) {
        'account' { if ($Expanded) { return 272 } else { return 144 } }
        # A testrun has no server.ini, so Servers paints its single placeholder
        # row: collapsed 32px body -> Standard's 112px minimum, expanded adds
        # the 6px gap + 40px details block -> 78px body -> 128px outer.
        'servers' { if ($Expanded) { return 128 } else { return 112 } }
        'ps'      { if ($Expanded) { return 392 } else { return 168 } }
    }
    return 48
}

# ---------------------------------------------------------------------------
# evidence / assertions
# ---------------------------------------------------------------------------

function New-DeskResult {
    return [pscustomobject]@{
        Name     = ''
        Pass     = $false
        Checks   = New-Object System.Collections.Generic.List[object]
        Evidence = New-Object System.Collections.Generic.List[string]
        Notes    = New-Object System.Collections.Generic.List[string]
    }
}

function Add-DeskCheck {
    param([object]$Result, [string]$Name, [bool]$Pass, [string]$Detail = '')
    $Result.Checks.Add([pscustomobject]@{ Name = $Name; Pass = $Pass; Detail = $Detail })
    $tag = if ($Pass) { 'PASS' } else { 'FAIL' }
    Write-Host ("[{0}] {1}{2}" -f $tag, $Name, $(if ($Detail) { "  ($Detail)" } else { '' }))
    return $Pass
}

function Add-DeskNote {
    param([object]$Result, [string]$Note)
    $Result.Notes.Add($Note)
    Write-Host "[note] $Note"
}

function Save-DeskResult {
    param([object]$Result, [string]$Path)
    $Result.Pass = -not ($Result.Checks | Where-Object { -not $_.Pass })
    $directory = Split-Path -Parent $Path
    if ($directory -and -not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    $json = [pscustomobject]@{
        name     = $Result.Name
        pass     = $Result.Pass
        checks   = @($Result.Checks | ForEach-Object { [pscustomobject]@{ name = $_.Name; pass = $_.Pass; detail = $_.Detail } })
        evidence = @($Result.Evidence)
        notes    = @($Result.Notes)
        at       = (Get-Date).ToString('s')
    } | ConvertTo-Json -Depth 8
    [System.IO.File]::WriteAllText($Path, $json, (New-Object System.Text.UTF8Encoding($false)))
    return $Result.Pass
}

function Test-DeskRectInside {
    param([object]$Inner, [object]$Outer)
    return ($Inner.X -ge $Outer.X -and $Inner.Y -ge $Outer.Y -and
            ($Inner.X + $Inner.W) -le ($Outer.X + $Outer.W) -and
            ($Inner.Y + $Inner.H) -le ($Outer.Y + $Outer.H))
}

function Get-DeskOverlap {
    param([object]$A, [object]$B, [int]$Gap = 12)
    $ax2 = $A.X + $A.W; $ay2 = $A.Y + $A.H
    $bx2 = $B.X + $B.W; $by2 = $B.Y + $B.H
    $overlapX = [math]::Min($ax2, $bx2) - [math]::Max($A.X, $B.X)
    $overlapY = [math]::Min($ay2, $by2) - [math]::Max($A.Y, $B.Y)
    return ($overlapX -gt -$Gap -and $overlapY -gt -$Gap)
}
