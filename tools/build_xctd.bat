@echo off
rem tekken6-nx: builds out\xctd.exe (the DATA3xx.BIN decoder used by tools\dump_tekken_shaders.py) with Visual Studio's
rem clang against the SDK's libmspack LZX decoder. Run from anywhere.
call "%~dp0pc_env.bat" || exit /b 1
set "ROOT=%~dp0.."
set "MSPACK=%ROOT%\sdk\thirdparty\libmspack\libmspack\mspack"
if not exist "%ROOT%\out" mkdir "%ROOT%\out"
"%LLVM_BIN%/clang.exe" -O2 -D_CRT_SECURE_NO_WARNINGS -w -I "%MSPACK%" "%~dp0xctd.c" "%MSPACK%\lzxd.c" -o "%ROOT%\out\xctd.exe" || exit /b 1
echo built %ROOT%\out\xctd.exe
