@echo off
rem Sets up the Windows host toolchain for building ReXGlue projects:
rem   - MSVC headers/libs + Windows SDK (vcvars64 of the newest Visual Studio found by vswhere)
rem   - the CMake and Ninja bundled with that Visual Studio
rem   - clang driving the MSVC ABI (-target x86_64-pc-windows-msvc), linked with lld-link
rem Overrides (environment, all optional): VSROOT (Visual Studio folder), LLVM_BIN (folder with clang.exe),
rem PYTHON_EXE (python.exe). Defaults: vswhere's newest install, its bundled LLVM, python on PATH.
rem Usage: call tools\pc_env.bat, then run cmake with %REX_CMAKE_COMPILERS%.
rem vswhere's folder holds "(x86)": kept in a variable so its parenthesis cannot close the for-in group.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if "%VSROOT%"=="" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%i"
if "%VSROOT%"=="" (echo pc_env: Visual Studio not found & exit /b 1)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
if "%LLVM_BIN%"=="" set "LLVM_BIN=%VSROOT%\VC\Tools\Llvm\x64\bin"
set "LLVM_BIN=%LLVM_BIN:\=/%"
if "%PYTHON_EXE%"=="" for /f "delims=" %%i in ('where python') do if not defined PYTHON_EXE set "PYTHON_EXE=%%i"
set "PYTHON_EXE=%PYTHON_EXE:\=/%"
rem Every path is quoted: the Visual Studio folder contains spaces.
set REX_CMAKE_COMPILERS="-DCMAKE_C_COMPILER=%LLVM_BIN%/clang.exe" "-DCMAKE_CXX_COMPILER=%LLVM_BIN%/clang++.exe" -DCMAKE_C_COMPILER_TARGET=x86_64-pc-windows-msvc -DCMAKE_CXX_COMPILER_TARGET=x86_64-pc-windows-msvc -DCMAKE_LINKER_TYPE=LLD "-DCMAKE_RC_COMPILER=%LLVM_BIN%/llvm-rc.exe" "-DCMAKE_AR=%LLVM_BIN%/llvm-ar.exe" "-DCMAKE_C_FLAGS=-march=x86-64-v2" "-DCMAKE_CXX_FLAGS=-march=x86-64-v2" "-DPYTHON_EXECUTABLE=%PYTHON_EXE%" "-DPython3_EXECUTABLE=%PYTHON_EXE%"
rem The code generator writes the loaded game image here (tools\post_codegen.py: ro_constants.py and the entry modes
rem of flush_mode_flow.py read it). Game data: never committed or distributed.
set "REXGLUE_DUMP_IMAGE=%~dp0..\out\renut_image.bin"
