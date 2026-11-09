// A console client for the ConPTY mouse round-trip test. Like Node's raw TUI
// input, it reads the VT byte stream rather than Win32 INPUT_RECORD events.
#include <windows.h>

#include <cstdio>
#include <string>

int main() {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const DWORD inputMode = ENABLE_EXTENDED_FLAGS | ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT |
                            ENABLE_VIRTUAL_TERMINAL_INPUT;
    if (!SetConsoleMode(in, inputMode))
        return 2;
    DWORD outputMode = 0;
    if (!GetConsoleMode(out, &outputMode) ||
        !SetConsoleMode(out, outputMode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
        return 3;
    std::printf("\x1b[?1000h\x1b[?1002h\x1b[?1006hMOUSE-READY\r\n");
    std::fflush(stdout);
    std::string received;
    for (;;) {
        char buffer[64];
        DWORD count = 0;
        if (!ReadFile(in, buffer, sizeof(buffer), &count, nullptr) || count == 0) return 6;
        received.append(buffer, count);
        if (received.find("\x1b[<80;12;4M") != std::string::npos &&
            received.find("\x1b[<73;60;10M") != std::string::npos) {
            std::printf("MOUSE-OK\r\n");
            std::fflush(stdout);
            return 0;
        }
        if (received.size() > 256) return 7;
    }
}
