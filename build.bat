@echo off
gcc -O2 main.c -o just.exe -lm -lws2_32
if %errorlevel% equ 0 (
    echo Build successful: just.exe
) else (
    echo Build failed
)