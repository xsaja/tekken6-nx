@echo off
rem Builds a target of the host SDK build (default: rexglue, the code generator). JOBS (env) sets the job count.
call "%~dp0pc_env.bat" || exit /b 1
if "%JOBS%"=="" set "JOBS=%NUMBER_OF_PROCESSORS%"
set "TARGET=%1"
if "%TARGET%"=="" set "TARGET=rexglue"
cmake --build "%~dp0..\out\host" --target %TARGET% -j %JOBS%
