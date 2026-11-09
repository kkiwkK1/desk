@echo off
rem Build the AI-mini experiment against the widget framework in ..\..\src.
rem Everything is compiled from relative paths: the checkout may live under a
rem non-ASCII directory and MinGW's assembler cannot open such paths.
setlocal enabledelayedexpansion
chcp 65001 >nul
cd /d "%~dp0"

set "TOOLROOT=C:\Tools\mingw64"
if exist "%TOOLROOT%\bin\g++.exe" set "PATH=%TOOLROOT%\bin;%PATH%"
where g++ >nul 2>nul || (echo [ai-mini] g++ not found & exit /b 1)

if not exist "build\obj" mkdir "build\obj"
if not exist "build\bin" mkdir "build\bin"
if not exist "build\tmp" mkdir "build\tmp"
set "TMP=build\tmp"
set "TEMP=build\tmp"

set "CXXFLAGS=-std=c++20 -O2 -Wall -Wextra -DNOMINMAX -DUNICODE -D_UNICODE -Isrc -I..\..\src"
set "LIBS=-static -static-libgcc -static-libstdc++ -lshell32 -luser32 -lgdi32 -lole32 -luuid -lwinhttp -ladvapi32 -limm32"
set "FW=canvas cjk view layout store http secrets host prompt proc"

rem --- framework, compiled here so the experiment stays self-contained -----
echo [ai-mini] framework
set "OBJ="
for %%M in (%FW%) do (
    g++ %CXXFLAGS% -c "..\..\src\widget\%%M.cpp" -o "build\obj\fw-%%M.o"
    if errorlevel 1 exit /b 1
    set "OBJ=!OBJ! "build\obj\fw-%%M.o""
)
if exist "build\libwg.a" del /q "build\libwg.a"
ar rcs "build\libwg.a" %OBJ%
if errorlevel 1 (echo [ai-mini] ar failed & exit /b 1)

rem --- experiment core ------------------------------------------------------
echo [ai-mini] core
set "CORE=json text endpoint apikey llm tools agent"
set "COBJ="
for %%M in (%CORE%) do (
    g++ %CXXFLAGS% -municode -c "src\%%M.cpp" -o "build\obj\ai-%%M.o"
    if errorlevel 1 exit /b 1
    set "COBJ=!COBJ! "build\obj\ai-%%M.o""
)

rem --- console front end ----------------------------------------------------
echo [ai-mini] cli
g++ %CXXFLAGS% -municode -c "src\cli.cpp" -o "build\obj\ai-cli.o"
if errorlevel 1 exit /b 1
g++ -municode -mconsole %COBJ% "build\obj\ai-cli.o" "build\libwg.a" -o "build\bin\ai-mini-cli.exe.new" %LIBS%
if errorlevel 1 exit /b 1
move /y "build\bin\ai-mini-cli.exe.new" "build\bin\ai-mini-cli.exe" >nul
if errorlevel 1 (echo [ai-mini] cannot replace ai-mini-cli.exe & exit /b 1)

rem --- desktop widget -------------------------------------------------------
echo [ai-mini] widget
g++ %CXXFLAGS% -municode -c "src\widget.cpp" -o "build\obj\ai-widget.o"
if errorlevel 1 exit /b 1
g++ -municode -mwindows %COBJ% "build\obj\ai-widget.o" "build\libwg.a" -o "build\bin\ai-mini.exe.new" %LIBS% -s
if errorlevel 1 exit /b 1
move /y "build\bin\ai-mini.exe.new" "build\bin\ai-mini.exe" >nul
if errorlevel 1 (echo [ai-mini] cannot replace ai-mini.exe & exit /b 1)

rem --- CJK rendering probe (toolsmeasure_cjk.cpp) ------------------------
if /i "%~1"=="bench" (
    echo [ai-mini] cjk probe
    g++ %CXXFLAGS% -municode -c "tools\measure_cjk.cpp" -o "build\obj\measure-cjk.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\measure-cjk.o" -o "build\measure-cjk.exe.new" %LIBS% -lpsapi
    if errorlevel 1 exit /b 1
    move /y "build\measure-cjk.exe.new" "build\measure-cjk.exe" >nul
    if errorlevel 1 exit /b 1
    "build\measure-cjk.exe"
    if errorlevel 1 exit /b 1
)

rem --- real-path render bench (Canvas + cjk::Text) ------------------------
if /i "%~1"=="renderbench" (
    echo [ai-mini] render bench
    g++ %CXXFLAGS% -municode -c "tools\render_bench.cpp" -o "build\obj\render-bench.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\render-bench.o" "build\libwg.a" -o "build\render-bench.exe.new" %LIBS% -lpsapi
    if errorlevel 1 exit /b 1
    move /y "build\render-bench.exe.new" "build\render-bench.exe" >nul
    if errorlevel 1 exit /b 1
    "build\render-bench.exe"
    if errorlevel 1 exit /b 1
)

rem --- PromptText / Chinese input probe -----------------------------------
if /i "%~1"=="prompttest" (
    echo [ai-mini] prompt probe
    g++ %CXXFLAGS% -municode -c "tools\prompt_probe.cpp" -o "build\obj\prompt-probe.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole "build\obj\prompt-probe.o" "build\libwg.a" -o "build\prompt-probe.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\prompt-probe.exe.new" "build\prompt-probe.exe" >nul
    if errorlevel 1 exit /b 1
    "build\prompt-probe.exe"
    if errorlevel 1 exit /b 1
)

rem --- offline tests --------------------------------------------------------
if /i "%~1"=="test" (
    echo [ai-mini] tests
    g++ %CXXFLAGS% -municode -c "tests\test_core.cpp" -o "build\obj\ai-test.o"
    if errorlevel 1 exit /b 1
    g++ -municode -mconsole %COBJ% "build\obj\ai-test.o" "build\libwg.a" -o "build\ai-mini-test.exe.new" %LIBS%
    if errorlevel 1 exit /b 1
    move /y "build\ai-mini-test.exe.new" "build\ai-mini-test.exe" >nul
    if errorlevel 1 exit /b 1
    "build\ai-mini-test.exe"
    if errorlevel 1 exit /b 1
)

echo [ai-mini] ok -^> build\bin
