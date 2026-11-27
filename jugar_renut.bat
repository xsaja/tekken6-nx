@echo off
rem tekken6-nx: abre Tekken 6 en PC con el renderer reNut (Vulkan), mando + teclado, con sonido.
rem Doble clic para jugar. Los shaders que falten se capturan solos en app\out\pc\shader_capture;
rem al cerrar el juego, doble clic en compilar_renut.bat para que entren en la siguiente version.
cd /d "%~dp0"
start "" "%~dp0app\out\pc\renut.exe" "--game_data_root=%~dp0..\game" "--user_data_root=%~dp0out\pc_user" --banjo_renderer=renut --renut_gpu_backend=vulkan --mnk_mode=true --fullscreen=false
