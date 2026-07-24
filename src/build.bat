@echo off
setlocal enabledelayedexpansion

echo ============================================
echo   Just Language - Build Script (Windows)
echo ============================================
echo.
echo This builds the CORE interpreter only. Plugins (like graphics)
echo are separate .dll files, built and loaded independently - this
echo script does not ask about them.
echo.

set CFLAGS=-std=c11 -Wall -Wextra -O2 -pthread -Wl,--export-all-symbols
set LIBS=-lws2_32 -lm
set INCLUDES=
set OUT=just.exe

REM ---------------- SQLite ----------------
set /p WANT_SQL="Include SQLite support? (y/n): "
if /i "%WANT_SQL%"=="y" (
    if not exist sqlite3.c (
        echo.
        echo ERROR: sqlite3.c not found in this folder.
        echo Download the SQLite amalgamation from https://sqlite.org/download.html
        echo and place sqlite3.c next to just.c, then run this script again.
        goto :end
    )
    echo   -^> SQLite: ON
) else (
    set CFLAGS=%CFLAGS% -DJUST_NO_SQLITE
    echo   -^> SQLite: OFF
)

REM ---------------- HTTPS (mbedTLS) ----------------
set /p WANT_TLS="Include HTTPS support? (y/n): "
if /i "%WANT_TLS%"=="y" (
    if not exist mbedtls\include (
        echo.
        echo ERROR: mbedtls\include folder not found.
        echo Download mbedTLS 2.28.x LTS from:
        echo   https://github.com/Mbed-TLS/mbedtls/releases
        echo Extract it so this folder contains: mbedtls\include and mbedtls\library
        echo Then build it once with:  cd mbedtls ^&^& mingw32-make lib
        echo and run this script again.
        goto :end
    )
    if not exist mbedtls\library\libmbedtls.a (
        echo.
        echo ERROR: mbedtls\library\libmbedtls.a not found.
        echo mbedTLS source is present but not built yet. Run:
        echo   cd mbedtls ^&^& mingw32-make lib
        echo then run this script again.
        goto :end
    )
    set INCLUDES=-Imbedtls\include
    set LIBS=-Lmbedtls\library -lmbedtls -lmbedx509 -lmbedcrypto %LIBS%
    echo   -^> HTTPS: ON
) else (
    set CFLAGS=%CFLAGS% -DJUST_NO_TLS
    echo   -^> HTTPS: OFF
)

echo.
echo Building %OUT% ...
gcc %CFLAGS% %INCLUDES% just.c main.c -o %OUT% %LIBS%

if errorlevel 1 (
    echo.
    echo Build FAILED. See errors above.
) else (
    echo.
    echo Build OK -^> %OUT%
    echo This is a single, self-contained executable - no other files needed
    echo to run it (unless you load a plugin .dll separately).
)

:end
echo.
pause
