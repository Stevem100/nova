@echo off
REM Build the Nova interpreter on Windows.
REM Requires either MinGW-w64 (gcc) or LLVM/Clang on PATH.
REM For MSVC (cl.exe in a Developer Command Prompt), use:
REM   cl /O2 /std:c11 /W4 /Fe:nova.exe src\*.c bcrypt.lib

where gcc >nul 2>nul
if %errorlevel%==0 (
    gcc -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -o nova.exe src\value.c src\gc.c src\lexer.c src\parser.c src\interp.c src\builtins.c src\web.c src\mail.c src\json.c src\crypto.c src\pkg.c src\main.c -lm -lbcrypt
    if %errorlevel%==0 echo Built nova.exe successfully.
    goto :eof
)

where clang >nul 2>nul
if %errorlevel%==0 (
    clang -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -o nova.exe src\value.c src\gc.c src\lexer.c src\parser.c src\interp.c src\builtins.c src\web.c src\mail.c src\json.c src\crypto.c src\pkg.c src\main.c -lm -lbcrypt
    if %errorlevel%==0 echo Built nova.exe successfully.
    goto :eof
)

echo No gcc or clang found on PATH. Install MinGW-w64 or run cl.exe from a VS Developer prompt.
exit /b 1
