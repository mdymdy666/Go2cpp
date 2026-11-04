@echo off
rem ============================================================================
rem rd_net 验收测试：Windows / MSVC 构建 + 运行（REQ-006 / REQ-007 / REQ-010）
rem
rem 用法（在 "x64 Native Tools Command Prompt for VS 2022" 里执行，或先 call
rem vcvars64.bat）：
rem
rem   tests\rd_net\build_and_run_windows.bat [库目录] [输出目录]
rem
rem   库目录：go2cpp_network.lib / go2cpp_io.lib / go2cpp_context.lib /
rem           go2cpp_error.lib 所在目录，默认 <repo>\build\lib。
rem   输出目录：.obj/.exe 输出目录，默认 <repo>\build\rd_net_tests。
rem
rem 说明：本脚本对应 Linux 侧的 build_and_run_*.sh；额外链接 ws2_32.lib
rem       （Winsock）与 psapi.lib（GetProcessMemoryInfo）。/W4 /WX 与 JinShan
rem       的验收口径一致：零警告即通过。
rem ============================================================================
setlocal
set "ROOT=%~dp0..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "LIBDIR=%~1"
if "%LIBDIR%"=="" set "LIBDIR=%ROOT%\build\lib"
set "OUTDIR=%~2"
if "%OUTDIR%"=="" set "OUTDIR=%ROOT%\build\rd_net_tests"

where cl >nul 2>nul
if errorlevel 1 (
  echo [ERROR] cl.exe not found in PATH.
  echo         Run this script in "x64 Native Tools Command Prompt for VS 2022",
  echo         or call vcvars64.bat first.
  exit /b 1
)

if not exist "%OUTDIR%" mkdir "%OUTDIR%"
pushd "%OUTDIR%"

set "CFLAGS=/nologo /std:c++17 /EHsc /O2 /W4 /WX /I"%ROOT%\include""
set "RDNLIB=go2cpp_network.lib go2cpp_io.lib go2cpp_context.lib go2cpp_error.lib"
set "WSLIB=ws2_32.lib psapi.lib"

echo ==== [1/4] test_reliability (header-only) ====
cl %CFLAGS% "%ROOT%\tests\rd_net\test_reliability.cpp"
if errorlevel 1 goto :fail
test_reliability.exe
if errorlevel 1 goto :fail

echo ==== [2/4] test_socket (REQ-006) ====
cl %CFLAGS% "%ROOT%\tests\rd_net\test_socket.cpp" "%ROOT%\src\rd_net\socket.cpp" ^
   /link /LIBPATH:"%LIBDIR%" %RDNLIB% %WSLIB%
if errorlevel 1 goto :fail
test_socket.exe
if errorlevel 1 goto :fail

echo ==== [3/4] test_event_loop (REQ-007) ====
cl %CFLAGS% "%ROOT%\tests\rd_net\test_event_loop.cpp" "%ROOT%\src\rd_net\socket.cpp" ^
   "%ROOT%\src\rd_net\event_loop.cpp" ^
   /link /LIBPATH:"%LIBDIR%" %RDNLIB% %WSLIB%
if errorlevel 1 goto :fail
test_event_loop.exe
if errorlevel 1 goto :fail

echo ==== [4/4] test_protocol (REQ-008) ====
cl %CFLAGS% "%ROOT%\tests\rd_net\test_protocol.cpp" "%ROOT%\src\rd_net\socket.cpp" ^
   "%ROOT%\src\rd_net\event_loop.cpp" "%ROOT%\src\rd_net\protocol.cpp" ^
   /link /LIBPATH:"%LIBDIR%" %RDNLIB% %WSLIB%
if errorlevel 1 goto :fail
test_protocol.exe
if errorlevel 1 goto :fail

echo.
echo [OK] all rd_net tests passed (MSVC /W4 /WX).
popd
exit /b 0

:fail
echo.
echo [FAIL] see log above.
popd
exit /b 1
