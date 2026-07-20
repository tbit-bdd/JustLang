@echo off
setlocal

set CFLAGS=-std=c11 -Wall -Wextra -O2
set LIBS=-lws2_32 -lm
set OUT=just.exe

set /p SQLITE="Include SQLite support? (0 = no, 1 = yes): "

if "%SQLITE%"=="1" (
    if not exist sqlite3.c (
        echo ERROR: sqlite3.c not found in this folder. Place it next to just.c or choose 0.
        goto :end
    )
    echo Building %OUT% WITH SQLite...
    gcc %CFLAGS% just.c main.c -o %OUT% %LIBS%
) else (
    echo Building %OUT% WITHOUT SQLite...
    gcc %CFLAGS% -DJUST_NO_SQLITE just.c main.c -o %OUT% %LIBS%
)

if errorlevel 1 (
    echo Build FAILED.
) else (
    echo Build OK -^> %OUT%
)

:end
pause
