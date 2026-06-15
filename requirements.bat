@echo off
setlocal enabledelayedexpansion

echo Checking for Git...
where git >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo ERROR: Git is not installed or not in PATH. Please install Git first.
    exit /b 1
)

:: Clone Dear ImGui
if not exist imgui (
    echo Downloading Dear ImGui...
    git clone --depth 1 https://github.com/ocornut/imgui.git imgui
    if !ERRORLEVEL! neq 0 (
        echo ERROR: Failed to clone Dear ImGui.
        exit /b !ERRORLEVEL!
    )
) else (
    echo imgui folder already exists, skipping download.
)

:: Clone lwext4
if not exist lwext4 (
    echo Downloading lwext4...
    git clone --depth 1 https://github.com/gkostka/lwext4.git lwext4
    if !ERRORLEVEL! neq 0 (
        echo ERROR: Failed to clone lwext4.
        exit /b !ERRORLEVEL!
    )

    echo Generating lwext4 configuration file...
    if not exist lwext4\include\generated mkdir lwext4\include\generated
    (
        echo #ifndef EXT4_CONFIG_H_
        echo #define EXT4_CONFIG_H_
        echo.
        echo #include ^<stdint.h^>
        echo #include ^<stddef.h^>
        echo #include ^<errno.h^>
        echo.
        echo #define CONFIG_DEBUG_PRINTF 0
        echo #define CONFIG_DEBUG_ASSERT 1
        echo #define CONFIG_BLOCK_DEV_CACHE_SIZE 16
        echo #define CONFIG_EXTENT_ENABLE 1
        echo #define CONFIG_XATTR_ENABLE 1
        echo #define CONFIG_JOURNALING_ENABLE 0
        echo #define CONFIG_HAVE_OWN_ERRNO 0
        echo #define CONFIG_HAVE_OWN_ASSERT 0
        echo.
        echo #ifndef EOK
        echo #define EOK 0
        echo #endif
        echo.
        echo #endif
    ) > lwext4\include\generated\ext4_config.h

    echo Patching lwext4 for dynamic disk/casefold support...
    powershell -Command "$p = 'lwext4\include\ext4_types.h'; $c = [System.IO.File]::ReadAllText($p); $c = $c.Replace('#define EXT4_FINCOM_INLINE_DATA 0x8000      /* data in inode */', '#define EXT4_FINCOM_INLINE_DATA 0x8000      /* data in inode */' + [System.Environment]::NewLine + '#define EXT4_FINCOM_CASEFOLD 0x20000        /* case-insensitive filenames */'); $c = $c.Replace('EXT4_FINCOM_RECOVER | EXT4_FINCOM_MMP', '(EXT4_FINCOM_RECOVER | EXT4_FINCOM_MMP | EXT4_FINCOM_CASEFOLD)'); [System.IO.File]::WriteAllText($p, $c)"
    
    if !ERRORLEVEL! equ 0 (
        echo lwext4 successfully patched.
    ) else (
        echo ERROR: Failed to patch lwext4.
        exit /b !ERRORLEVEL!
    )
) else (
    echo lwext4 folder already exists, skipping download.
)

echo.
echo Requirements setup complete! You can now run build.bat.
exit /b 0
