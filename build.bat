@echo off
setlocal enabledelayedexpansion

set OUT_DIR=build
set OBJ_DIR=%OUT_DIR%\obj

:: Check if clean is requested
if "%~1"=="clean" (
    echo Cleaning build artifacts...
    if exist %OUT_DIR% (
        rmdir /s /q %OUT_DIR%
        echo Clean completed successfully.
    ) else (
        echo Already clean.
    )
    exit /b 0
)

:: Ensure build directories exist
if not exist %OUT_DIR% mkdir %OUT_DIR%
if not exist %OBJ_DIR% mkdir %OBJ_DIR%

echo Building Virtual Ext4 Explorer...

:: Initialize MSVC environment if cl is not in path
where cl >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo Initializing MSVC environment...
    set "VS_PATH="
    if exist "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" (
        for /f "usebackq tokens=*" %%i in (`"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do (
            set "VS_PATH=%%i"
        )
    )
    if defined VS_PATH (
        call "!VS_PATH!\VC\Auxiliary\Build\vcvarsall.bat" x64
    ) else (
        echo Visual Studio installation not found via vswhere. Trying default path...
        call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
    )
)

:: Re-check if MSVC tools are now in the path
where cl >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo ERROR: cl.exe not found. Please ensure Visual Studio C++ build tools are installed.
    exit /b 1
)

:: Compile resources
rc /nologo /fo%OBJ_DIR%\resource.res src\resource.rc
if %ERRORLEVEL% neq 0 (
    echo ERROR: Resource compilation failed.
    exit /b %ERRORLEVEL%
)

set INCLUDES=/I"src" /I"imgui" /I"lwext4/include" /I"lwext4/include/generated" /I"lwext4/blockdev/windows"
set SOURCES=src\main.cpp src\VHDManager.cpp imgui\imgui.cpp imgui\imgui_demo.cpp imgui\imgui_draw.cpp imgui\imgui_tables.cpp imgui\imgui_widgets.cpp imgui\misc\cpp\imgui_stdlib.cpp imgui\backends\imgui_impl_win32.cpp imgui\backends\imgui_impl_dx11.cpp lwext4\src\*.c lwext4\blockdev\windows\*.c
set LIBS=user32.lib gdi32.lib shell32.lib d3d11.lib d3dcompiler.lib virtdisk.lib comdlg32.lib ole32.lib

cl /nologo /O1 /MT /D_CRT_SECURE_NO_WARNINGS %INCLUDES% %SOURCES% %OBJ_DIR%\resource.res %LIBS% /Fe%OUT_DIR%\VirtualExt4Explorer.exe /Fo%OBJ_DIR%\ /link /SUBSYSTEM:WINDOWS

if %ERRORLEVEL% equ 0 (
    echo SUCCESS: VirtualExt4Explorer.exe built.
    exit /b 0
) else (
    echo FAILED: errorlevel %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)