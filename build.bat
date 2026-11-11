@echo off
rem Some nested cmd exit /b paths report zero after an inner failure on this
rem host. Only the explicit completion marker may make the outer gate succeed.
if defined DESK_BUILD_GATE_INNER goto build_inner
setlocal
set "DESK_BUILD_GATE_INNER=1"
set "DESK_BUILD_GATE_OK="
call "%~f0" %*
if defined DESK_BUILD_GATE_OK exit /b 0
exit /b 1

:build_inner
setlocal enabledelayedexpansion
chcp 65001 >nul
cd /d "%~dp0"

rem MinGW-w64 GCC. Set TOOLROOT before running this script to use another install.
if not defined TOOLROOT set "TOOLROOT=C:\Tools\mingw64"
if exist "%TOOLROOT%\bin\g++.exe" set "PATH=%TOOLROOT%\bin;%PATH%"
where g++ >nul 2>nul || (echo [build] g++ not found & exit /b 1)

if not "%~1"=="" if /i not "%~1"=="test" if /i not "%~1"=="framework" if /i not "%~1"=="example" (
    echo Usage: build.bat [framework^|example^|test]
    exit /b 1
)

rem Keep paths relative and pipe compiler output to the assembler: MinGW's
rem assembler cannot open temporary files under some non-ASCII paths.
if not exist "build\obj" mkdir "build\obj"
if not exist "build\bin" mkdir "build\bin"
if not exist "build\tmp" mkdir "build\tmp"
set "TMP=build\tmp"
set "TEMP=build\tmp"

set "BASEFLAGS=-std=c++20 -O2 -pipe -Wall -Wextra -DNOMINMAX -DUNICODE -D_UNICODE"
set "CXXFLAGS=%BASEFLAGS% -Isrc"
set "SDKFLAGS=%BASEFLAGS% -Ibuild/sdk/include"
set "LIBS=-static -static-libgcc -static-libstdc++ -lshell32 -luser32 -lgdi32 -lole32 -luuid -lwinhttp -ladvapi32 -limm32 -lversion -lws2_32"
set "FW=canvas cjk view layout store http secrets prompt proc conpty skin board board_layout board_plan board_grow display diag appenv"

echo [build] framework
set "OBJ="
for %%M in (%FW%) do (
    echo   - %%M
    g++ %CXXFLAGS% -c "src\widget\%%M.cpp" -o "build\obj\%%M.o"
    if errorlevel 1 exit /b 1
    set "OBJ=!OBJ! "build\obj\%%M.o""
)
if exist "build\libwg.a" del /q "build\libwg.a"
ar rcs "build\libwg.a" %OBJ%
if errorlevel 1 (echo [build] ar failed & exit /b 1)

rem Export only framework headers and library. A consumer needs neither src
rem nor any of the desk panels/data sources. Remove old exported headers before
rem copying so a removed API is not accidentally retained in a rebuilt SDK.
if not exist "build\sdk\include\widget" mkdir "build\sdk\include\widget"
if not exist "build\sdk\lib" mkdir "build\sdk\lib"
if exist "build\sdk\include\widget\*.h" (
    del /q "build\sdk\include\widget\*.h"
    if errorlevel 1 (echo [build] cannot clear old framework headers & exit /b 1)
)
copy /y "src\widget\*.h" "build\sdk\include\widget\" >nul
if errorlevel 1 (echo [build] cannot export framework headers & exit /b 1)
copy /y "build\libwg.a" "build\sdk\lib\libwg.a" >nul
if errorlevel 1 (echo [build] cannot export framework library & exit /b 1)

if /i "%~1"=="framework" (
    echo [build] framework SDK -^> build\sdk
    endlocal & set "DESK_BUILD_GATE_OK=1" & exit /b 0
)

rem Build the independent consumer exclusively against the exported SDK.
if /i "%~1"=="example" (
    echo [build] framework example
    if not exist "examples\minimal-board\main.cpp" (echo [build] example source not found & exit /b 1)
    if not exist "build\obj\example" mkdir "build\obj\example"
    set "EOBJ="
    for %%F in ("examples\minimal-board\*.cpp") do (
        g++ %SDKFLAGS% -municode -c "%%F" -o "build\obj\example\%%~nF.o"
        if errorlevel 1 exit /b 1
        set "EOBJ=!EOBJ! "build\obj\example\%%~nF.o""
    )
    g++ -municode -mwindows !EOBJ! "build\sdk\lib\libwg.a" -o "build\bin\minimal-board.exe.new" %LIBS% -s
    if errorlevel 1 exit /b 1
    move /y "build\bin\minimal-board.exe.new" "build\bin\minimal-board.exe" >nul
    if errorlevel 1 (echo [build] cannot replace minimal-board.exe & exit /b 1)
    echo [build] example -^> build\bin\minimal-board.exe
    endlocal & set "DESK_BUILD_GATE_OK=1" & exit /b 0
)

echo [build] desk
if not exist "build\obj\desk" mkdir "build\obj\desk"
if not exist "build\obj\desk\aim" mkdir "build\obj\desk\aim"
if not exist "build\obj\desk\srv" mkdir "build\obj\desk\srv"
if not exist "build\obj\desk\ps" mkdir "build\obj\desk\ps"
set "DOBJ="
for %%F in ("src\apps\desk\*.cpp") do (
    g++ %CXXFLAGS% -municode -c "%%F" -o "build\obj\desk\%%~nF.o"
    if errorlevel 1 exit /b 1
    set "DOBJ=!DOBJ! "build\obj\desk\%%~nF.o""
)
set "PANELCOUNT=0"
for /d %%D in (src\panels\*) do (
    set "PNAME=%%~nxD"
    echo   - !PNAME!
    if not exist "build\obj\panels\!PNAME!" mkdir "build\obj\panels\!PNAME!"
    for %%F in ("%%D\*.cpp") do (
        g++ %CXXFLAGS% -municode -c "%%F" -o "build\obj\panels\!PNAME!\%%~nF.o"
        if errorlevel 1 exit /b 1
        set "DOBJ=!DOBJ! "build\obj\panels\!PNAME!\%%~nF.o""
    )
    set /a PANELCOUNT+=1
)
if "!PANELCOUNT!"=="0" (echo [build] no panels under src\panels & exit /b 1)

rem Shared data sources: the cards and srvctl use the same code.
for %%M in (providers token_usage commandcode collect accounts history) do (
    g++ %CXXFLAGS% -c "src\data\aimeter\%%M.cpp" -o "build\obj\desk\aim\%%M.o"
    if errorlevel 1 exit /b 1
    set "DOBJ=!DOBJ! "build\obj\desk\aim\%%M.o""
)
rem Assistant runtime: memory/context now; json/pi_client/engine join as they land.
if not exist "build\obj\desk\asm" mkdir "build\obj\desk\asm"
for %%M in (json pi_client memory context engine) do (
    g++ %CXXFLAGS% -c "src\data\assistant\%%M.cpp" -o "build\obj\desk\asm\%%M.o"
    if errorlevel 1 exit /b 1
    set "DOBJ=!DOBJ! "build\obj\desk\asm\%%M.o""
)
rem M2.0 CardSpec: application-layer declarative inline data (not framework SDK).
if not exist "build\obj\desk\csp" mkdir "build\obj\desk\csp"
g++ %CXXFLAGS% -c "src\data\cardspec\cardspec.cpp" -o "build\obj\desk\csp\cardspec.o"
if errorlevel 1 exit /b 1
set "DOBJ=!DOBJ! "build\obj\desk\csp\cardspec.o""
for %%M in (http_source http_policy http_winhttp store) do (
    g++ %CXXFLAGS% -c "src\data\cardspec\%%M.cpp" -o "build\obj\desk\csp\%%M.o"
    if errorlevel 1 exit /b 1
    set "DOBJ=!DOBJ! "build\obj\desk\csp\%%M.o""
)
for %%M in (providers status_view collect) do (
    g++ %CXXFLAGS% -c "src\data\server\%%M.cpp" -o "build\obj\desk\srv\%%M.o"
    if errorlevel 1 exit /b 1
    set "DOBJ=!DOBJ! "build\obj\desk\srv\%%M.o""
)
rem The PowerShell card: pipes, decoding, ANSI, line editing.
for %%M in (text shell selection screen vt terminal keymap pi_process) do (
    g++ %CXXFLAGS% -c "src\data\ps\%%M.cpp" -o "build\obj\desk\ps\%%M.o"
    if errorlevel 1 exit /b 1
    set "DOBJ=!DOBJ! "build\obj\desk\ps\%%M.o""
)
g++ -municode -mwindows !DOBJ! "build\libwg.a" -o "build\bin\desk.exe.new" %LIBS% -s
if errorlevel 1 exit /b 1
move /y "build\bin\desk.exe.new" "build\bin\desk.exe" >nul 2>nul
if errorlevel 1 (
    rem A running dev instance keeps the image mapped; renaming the old file
    rem still works, so the freshly built one can take its place.
    echo [build] desk.exe is in use - moving the old binary aside
    if exist "build\bin\desk.exe.old" del /q "build\bin\desk.exe.old" >nul 2>nul
    move /y "build\bin\desk.exe" "build\bin\desk.exe.old" >nul 2>nul
    if errorlevel 1 move /y "build\bin\desk.exe" "build\bin\desk.exe.old%RANDOM%%RANDOM%" >nul 2>nul
    if errorlevel 1 (echo [build] cannot release desk.exe & exit /b 1)
    move /y "build\bin\desk.exe.new" "build\bin\desk.exe" >nul
)
if errorlevel 1 (echo [build] cannot replace desk.exe & exit /b 1)

echo [build] cli
if not exist "build\obj\cli" mkdir "build\obj\cli"
set "COBJ="
for %%F in ("src\cli\*.cpp") do (
    g++ %CXXFLAGS% -municode -c "%%F" -o "build\obj\cli\%%~nF.o"
    if errorlevel 1 exit /b 1
    set "COBJ=!COBJ! "build\obj\cli\%%~nF.o""
)
g++ %CXXFLAGS% -c "src\data\server\providers.cpp" -o "build\obj\cli\providers.o"
if errorlevel 1 exit /b 1
g++ -municode -mconsole !COBJ! "build\obj\cli\providers.o" "build\libwg.a" -o "build\bin\srvctl.exe.new" %LIBS% -s
if errorlevel 1 exit /b 1
move /y "build\bin\srvctl.exe.new" "build\bin\srvctl.exe" >nul
if errorlevel 1 (echo [build] cannot replace srvctl.exe & exit /b 1)

if /i "%~1"=="test" (
    echo [build] tests
    if not exist "build\obj\test" mkdir "build\obj\test"

    echo   - render
    g++ %CXXFLAGS% -municode -c "tests\render.cpp" -o "build\obj\test\render.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\render.o" "build\libwg.a" -o "build\render-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\render-test.exe.new" "build\render-test.exe" >nul
    if errorlevel 1 exit /b 1

    echo   - board layout
    g++ %CXXFLAGS% -municode -c "tests\board_layout.cpp" -o "build\obj\test\board_layout.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\board_layout.o" "build\libwg.a" -o "build\board-layout-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\board-layout-test.exe.new" "build\board-layout-test.exe" >nul
    if errorlevel 1 exit /b 1

    echo   - board plan
    g++ %CXXFLAGS% -municode -c "tests\board_plan.cpp" -o "build\obj\test\board_plan.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\board_plan.o" "build\libwg.a" -o "build\board-plan-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\board-plan-test.exe.new" "build\board-plan-test.exe" >nul
    if errorlevel 1 exit /b 1

    echo   - size presets
    g++ %CXXFLAGS% -c "tests\size_presets.cpp" -o "build\obj\test\size_presets.o"
    if errorlevel 1 exit /b 1
    g++ -mconsole "build\obj\test\size_presets.o" "build\libwg.a" -o "build\size-presets-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\size-presets-test.exe.new" "build\size-presets-test.exe" >nul
    if errorlevel 1 exit /b 1

    echo   - board grow
    g++ %CXXFLAGS% -municode -c "tests\board_grow.cpp" -o "build\obj\test\board_grow.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\board_grow.o" "build\libwg.a" -o "build\board-grow-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\board-grow-test.exe.new" "build\board-grow-test.exe" >nul
    if errorlevel 1 exit /b 1

    echo   - board composition
    g++ %CXXFLAGS% -municode -c "tests\board_composition.cpp" -o "build\obj\test\board_composition.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\board_composition.o" "build\libwg.a" -o "build\board-composition-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\board-composition-test.exe.new" "build\board-composition-test.exe" >nul
    if errorlevel 1 exit /b 1

    echo   - module body
    g++ %CXXFLAGS% -municode -c "tests\module_body.cpp" -o "build\obj\test\module_body.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\module_body.o" "build\libwg.a" -o "build\module-body-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\module-body-test.exe.new" "build\module-body-test.exe" >nul
    if errorlevel 1 exit /b 1

    echo   - display source
    g++ %CXXFLAGS% -municode -c "tests\display_source.cpp" -o "build\obj\test\display_source.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\display_source.o" "build\libwg.a" -o "build\display-source-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\display-source-test.exe.new" "build\display-source-test.exe" >nul
    if errorlevel 1 exit /b 1

    echo   - diag
    g++ %CXXFLAGS% -municode -c "tests\diag.cpp" -o "build\obj\test\diag.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\diag.o" "build\libwg.a" -o "build\diag-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\diag-test.exe.new" "build\diag-test.exe" >nul
    if errorlevel 1 exit /b 1

    echo   - board host
    g++ %CXXFLAGS% -municode -c "tests\board_host.cpp" -o "build\obj\test\board_host.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\board_host.o" "build\libwg.a" -o "build\board-host-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\board-host-test.exe.new" "build\board-host-test.exe" >nul
    if errorlevel 1 exit /b 1

    echo   - store snapshot
    g++ %CXXFLAGS% -municode -c "tests\store_tests.cpp" -o "build\obj\test\store_tests.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\store_tests.o" "build\libwg.a" -o "build\store-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\store-test.exe.new" "build\store-test.exe" >nul
    if errorlevel 1 exit /b 1

    echo   - assistant memory
    g++ %CXXFLAGS% -municode -c "tests\assistant_memory.cpp" -o "build\obj\test\assistant_memory.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\data\assistant\memory.cpp" -o "build\obj\test\assistant_memory_data.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\assistant_memory.o" "build\obj\test\assistant_memory_data.o" "build\libwg.a" -o "build\assistant-memory-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\assistant-memory-test.exe.new" "build\assistant-memory-test.exe" >nul
    if errorlevel 1 exit /b 1

    echo   - assistant context
    g++ %CXXFLAGS% -municode -c "tests\assistant_context.cpp" -o "build\obj\test\assistant_context.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\data\assistant\context.cpp" -o "build\obj\test\assistant_context_data.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\assistant_context.o" "build\obj\test\assistant_memory_data.o" "build\obj\test\assistant_context_data.o" "build\libwg.a" -o "build\assistant-context-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\assistant-context-test.exe.new" "build\assistant-context-test.exe" >nul
    if errorlevel 1 exit /b 1

    echo   - pi client
    g++ %CXXFLAGS% -municode -c "src\data\assistant\json.cpp" -o "build\obj\test\assistant_json.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\data\assistant\pi_client.cpp" -o "build\obj\test\assistant_pi_client.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\data\assistant\engine.cpp" -o "build\obj\test\assistant_engine.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "tests\fake_pi_rpc.cpp" -o "build\obj\test\fake_pi_rpc.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\fake_pi_rpc.o" "build\obj\test\assistant_json.o" "build\libwg.a" -o "build\fake-pi-rpc.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\fake-pi-rpc.exe.new" "build\fake-pi-rpc.exe" >nul
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "tests\pi_client.cpp" -o "build\obj\test\pi_client.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\pi_client.o" "build\obj\test\assistant_pi_client.o" "build\obj\test\assistant_json.o" "build\libwg.a" -o "build\pi-client-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\pi-client-test.exe.new" "build\pi-client-test.exe" >nul
    if errorlevel 1 exit /b 1

    echo   - terminal
    g++ %CXXFLAGS% -municode -c "src\data\ps\text.cpp" -o "build\obj\test\ps_text.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\data\ps\shell.cpp" -o "build\obj\test\ps_shell.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\data\ps\selection.cpp" -o "build\obj\test\ps_selection.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\data\ps\screen.cpp" -o "build\obj\test\ps_screen.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\data\ps\vt.cpp" -o "build\obj\test\ps_vt.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\data\ps\terminal.cpp" -o "build\obj\test\ps_terminal.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\data\ps\keymap.cpp" -o "build\obj\test\ps_keymap.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\data\ps\pi_process.cpp" -o "build\obj\test\ps_pi_process.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "tests\ps_tests.cpp" -o "build\obj\test\ps_tests.o"
    if errorlevel 1 exit /b 1
    rem Windowless helper: ConPTY only attaches a client when the caller has no console.
    g++ %CXXFLAGS% -municode -c "tests\pty_helper.cpp" -o "build\obj\test\pty_helper.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -c "tests\pty_mouse_client.cpp" -o "build\obj\test\pty_mouse_client.o"
    if errorlevel 1 exit /b 1
    g++ -mconsole "build\obj\test\pty_mouse_client.o" -o "build\pty-mouse-client.exe.new" -static -static-libgcc -static-libstdc++
    if errorlevel 1 exit /b 1
    move /y "build\pty-mouse-client.exe.new" "build\pty-mouse-client.exe" >nul
    if errorlevel 1 exit /b 1
    g++ -municode -mwindows "build\obj\test\pty_helper.o" "build\obj\test\ps_text.o" "build\obj\test\ps_shell.o" "build\obj\test\ps_screen.o" "build\obj\test\ps_vt.o" "build\obj\test\ps_terminal.o" "build\obj\test\ps_keymap.o" "build\obj\test\ps_selection.o" "build\libwg.a" -o "build\pty-helper.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\pty-helper.exe.new" "build\pty-helper.exe" >nul
    if errorlevel 1 exit /b 1
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\ps_tests.o" "build\obj\test\ps_text.o" "build\obj\test\ps_shell.o" "build\obj\test\ps_selection.o" "build\obj\test\ps_screen.o" "build\obj\test\ps_vt.o" "build\obj\test\ps_terminal.o" "build\obj\test\ps_keymap.o" "build\obj\test\ps_pi_process.o" "build\libwg.a" -o "build\ps-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\ps-test.exe.new" "build\ps-test.exe" >nul

    echo   - aimeter accounts
    g++ %CXXFLAGS% -municode -c "tests\aimeter_accounts.cpp" -o "build\obj\test\aimeter_accounts.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\data\aimeter\accounts.cpp" -o "build\obj\test\aimeter_accounts_data.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\data\aimeter\history.cpp" -o "build\obj\test\aimeter_history.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\data\aimeter\token_usage.cpp" -o "build\obj\test\aimeter_token_usage.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\aimeter_accounts.o" "build\obj\test\aimeter_accounts_data.o" "build\obj\test\aimeter_history.o" "build\obj\test\aimeter_token_usage.o" "build\libwg.a" -o "build\aimeter-accounts-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\aimeter-accounts-test.exe.new" "build\aimeter-accounts-test.exe" >nul
    if errorlevel 1 exit /b 1

    echo   - assistant engine integration
    g++ %CXXFLAGS% -municode -c "tests\assistant_engine.cpp" -o "build\obj\test\assistant_engine_test.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\assistant_engine_test.o" "build\obj\test\assistant_engine.o" "build\obj\test\assistant_pi_client.o" "build\obj\test\assistant_json.o" "build\obj\test\assistant_memory_data.o" "build\obj\test\assistant_context_data.o" "build\obj\test\aimeter_accounts_data.o" "build\obj\test\aimeter_token_usage.o" "build\libwg.a" -o "build\assistant-engine-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\assistant-engine-test.exe.new" "build\assistant-engine-test.exe" >nul
    if errorlevel 1 exit /b 1

    echo   - desk render
    g++ %CXXFLAGS% -municode -c "src\panels\assistant\assistant.cpp" -o "build\obj\test\assistant_view.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\panels\memory\memory.cpp" -o "build\obj\test\memory_view.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\panels\account\account_view.cpp" -o "build\obj\test\account_view.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\panels\servers\servers_view.cpp" -o "build\obj\test\servers_view.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\data\server\status_view.cpp" -o "build\obj\test\status_view.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\data\server\providers.cpp" -o "build\obj\test\server_providers.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\data\aimeter\token_usage.cpp" -o "build\obj\test\token_usage.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\data\aimeter\providers.cpp" -o "build\obj\test\aimeter_providers.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\panels\ps\ps_view.cpp" -o "build\obj\test\ps_view.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "tests\desk_render.cpp" -o "build\obj\test\desk_render.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\desk_render.o" "build\obj\test\assistant_view.o" "build\obj\test\memory_view.o" "build\obj\test\assistant_engine.o" "build\obj\test\assistant_pi_client.o" "build\obj\test\assistant_json.o" "build\obj\test\assistant_memory_data.o" "build\obj\test\assistant_context_data.o" "build\obj\test\account_view.o" "build\obj\test\servers_view.o" "build\obj\test\ps_view.o" "build\obj\test\ps_selection.o" "build\obj\test\ps_screen.o" "build\obj\test\ps_vt.o" "build\obj\test\status_view.o" "build\obj\test\server_providers.o" "build\obj\test\token_usage.o" "build\obj\test\aimeter_providers.o" "build\obj\test\aimeter_accounts_data.o" "build\obj\test\aimeter_history.o" "build\libwg.a" -o "build\desk-render-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\desk-render-test.exe.new" "build\desk-render-test.exe" >nul
    if errorlevel 1 exit /b 1

    if errorlevel 1 exit /b 1

    echo   - cli
    g++ %CXXFLAGS% -municode -c "src\cli\args.cpp" -o "build\obj\test\args.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\cli\config.cpp" -o "build\obj\test\config.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\cli\output.cpp" -o "build\obj\test\output.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "src\cli\targets.cpp" -o "build\obj\test\targets.o"
    if errorlevel 1 exit /b 1
    g++ %CXXFLAGS% -municode -c "tests\cli_tests.cpp" -o "build\obj\test\cli_tests.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\args.o" "build\obj\test\config.o" "build\obj\test\output.o" "build\obj\test\targets.o" "build\obj\test\cli_tests.o" "build\libwg.a" -o "build\cli-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\cli-test.exe.new" "build\cli-test.exe" >nul
    if errorlevel 1 exit /b 1

    echo   - cardspec core
    g++ %CXXFLAGS% -municode -c "tests\cardspec.cpp" -o "build\obj\test\cardspec.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\cardspec.o" "build\obj\desk\csp\cardspec.o" "build\obj\test\assistant_json.o" "build\libwg.a" -o "build\cardspec-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\cardspec-test.exe.new" "build\cardspec-test.exe" >nul
    if errorlevel 1 exit /b 1

    echo   - cardspec render
    g++ %CXXFLAGS% -municode -c "tests\cardspec_render.cpp" -o "build\obj\test\cardspec_render.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\cardspec_render.o" "build\obj\panels\cardspec\cardspec.o" "build\obj\panels\cardspec\cardspec_view.o" "build\obj\desk\csp\http_source.o" "build\obj\desk\csp\http_policy.o" "build\obj\desk\csp\http_winhttp.o" "build\obj\desk\csp\cardspec.o" "build\obj\test\assistant_json.o" "build\libwg.a" -o "build\cardspec-render-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\cardspec-render-test.exe.new" "build\cardspec-render-test.exe" >nul
    if errorlevel 1 exit /b 1

    echo   - cardspec HTTP source bindings
    g++ %CXXFLAGS% -municode -c "tests\cardspec_source.cpp" -o "build\obj\test\cardspec_source.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\cardspec_source.o" "build\obj\desk\csp\http_source.o" "build\obj\desk\csp\cardspec.o" "build\obj\test\assistant_json.o" "build\libwg.a" -o "build\cardspec-source-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\cardspec-source-test.exe.new" "build\cardspec-source-test.exe" >nul
    if errorlevel 1 exit /b 1

    echo   - cardspec HTTPS policy
    g++ %CXXFLAGS% -municode -c "tests\cardspec_http.cpp" -o "build\obj\test\cardspec_http.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\cardspec_http.o" "build\obj\desk\csp\http_source.o" "build\obj\desk\csp\http_policy.o" "build\obj\desk\csp\http_winhttp.o" "build\obj\desk\csp\cardspec.o" "build\obj\test\assistant_json.o" "build\libwg.a" -o "build\cardspec-http-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\cardspec-http-test.exe.new" "build\cardspec-http-test.exe" >nul
    if errorlevel 1 exit /b 1

    echo   - cardspec network UI
    g++ %CXXFLAGS% -municode -c "tests\cardspec_network.cpp" -o "build\obj\test\cardspec_network.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\cardspec_network.o" "build\obj\panels\cardspec\cardspec.o" "build\obj\panels\cardspec\cardspec_view.o" "build\obj\desk\csp\http_source.o" "build\obj\desk\csp\http_policy.o" "build\obj\desk\csp\http_winhttp.o" "build\obj\desk\csp\cardspec.o" "build\obj\test\assistant_json.o" "build\libwg.a" -o "build\cardspec-network-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\cardspec-network-test.exe.new" "build\cardspec-network-test.exe" >nul
    if errorlevel 1 exit /b 1

    echo   - cardspec proposal store
    g++ %CXXFLAGS% -municode -c "tests\cardspec_store.cpp" -o "build\obj\test\cardspec_store.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\test\cardspec_store.o" "build\obj\desk\csp\store.o" "build\obj\desk\csp\cardspec.o" "build\obj\test\assistant_json.o" "build\libwg.a" -o "build\cardspec-store-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\cardspec-store-test.exe.new" "build\cardspec-store-test.exe" >nul
    if errorlevel 1 exit /b 1

    echo [build] sign tests
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\sign-local.ps1 -Path "build\*-test.exe"
    if errorlevel 1 (echo [build] test signing failed & exit /b 1)
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\sign-local.ps1 -Path "build\fake-pi-rpc.exe"
    if errorlevel 1 (echo [build] fake signing failed & exit /b 1)
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\sign-local.ps1 -Path "build\pty-helper.exe"
    if errorlevel 1 (echo [build] helper signing failed & exit /b 1)
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\sign-local.ps1 -Path "build\pty-mouse-client.exe"
    if errorlevel 1 (echo [build] mouse client signing failed & exit /b 1)

    echo [build] run tests
    "build\render-test.exe" "build\preview.bmp"
    if errorlevel 1 exit /b 1
    "build\board-layout-test.exe"
    if errorlevel 1 exit /b 1
    "build\board-plan-test.exe"
    if errorlevel 1 exit /b 1
    "build\board-grow-test.exe"
    if errorlevel 1 exit /b 1
    "build\size-presets-test.exe"
    if errorlevel 1 exit /b 1
    "build\board-composition-test.exe"
    if errorlevel 1 exit /b 1
    "build\store-test.exe"
    if errorlevel 1 exit /b 1
    "build\assistant-memory-test.exe"
    if errorlevel 1 exit /b 1
    "build\assistant-context-test.exe"
    if errorlevel 1 exit /b 1
    "build\pi-client-test.exe"
    if errorlevel 1 exit /b 1
    "build\assistant-engine-test.exe"
    if errorlevel 1 exit /b 1
    "build\module-body-test.exe"
    if errorlevel 1 exit /b 1
    "build\display-source-test.exe"
    if errorlevel 1 exit /b 1
    "build\diag-test.exe"
    if errorlevel 1 exit /b 1
    "build\board-host-test.exe"
    if errorlevel 1 exit /b 1
    "build\desk-render-test.exe" "build\desk-board-preview.bmp"
    if errorlevel 1 exit /b 1
    "build\ps-test.exe"
    if errorlevel 1 exit /b 1
    "build\cli-test.exe"
    if errorlevel 1 exit /b 1
    "build\aimeter-accounts-test.exe"
    if errorlevel 1 exit /b 1
    "build\cardspec-test.exe"
    if errorlevel 1 exit /b 1
    "build\cardspec-render-test.exe"
    if errorlevel 1 exit /b 1
    "build\cardspec-source-test.exe"
    if errorlevel 1 exit /b 1
    "build\cardspec-http-test.exe"
    if errorlevel 1 exit /b 1
    "build\cardspec-network-test.exe"
    if errorlevel 1 exit /b 1
    "build\cardspec-store-test.exe"
    if errorlevel 1 exit /b 1

    echo [build] source inventory
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\check-sources.ps1
    if errorlevel 1 exit /b 1
)

echo [build] ok -^> build\bin
endlocal & set "DESK_BUILD_GATE_OK=1" & exit /b 0
