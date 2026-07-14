if (NOT WIN32)
    return()
endif()

if (NOT DEFINED TARGET_FILE OR NOT EXISTS "${TARGET_FILE}")
    message(FATAL_ERROR "TARGET_FILE is missing for GUI deployment")
endif()

if (NOT DEFINED OUTPUT_DIR OR NOT IS_DIRECTORY "${OUTPUT_DIR}")
    message(FATAL_ERROR "OUTPUT_DIR is missing for GUI deployment")
endif()

set(QT_DEPLOYED FALSE)

if (DEFINED WINDEPLOYQT_EXECUTABLE AND EXISTS "${WINDEPLOYQT_EXECUTABLE}")
    execute_process(
        COMMAND "${WINDEPLOYQT_EXECUTABLE}"
            --release
            --no-translations
            --no-system-d3d-compiler
            --no-opengl-sw
            "${TARGET_FILE}"
        RESULT_VARIABLE deploy_result
    )
    if (deploy_result EQUAL 0)
        set(QT_DEPLOYED TRUE)
    else()
        message(WARNING "windeployqt failed with exit code ${deploy_result}; falling back to direct Qt runtime copy")
    endif()
else()
    message(WARNING "windeployqt was not found; falling back to direct Qt runtime copy")
endif()

if (NOT DEFINED QT_BIN_DIR OR NOT IS_DIRECTORY "${QT_BIN_DIR}")
    message(FATAL_ERROR "QT_BIN_DIR is missing; cannot deploy Qt runtime")
endif()

get_filename_component(QT_ROOT_DIR "${QT_BIN_DIR}" DIRECTORY)
set(QT_PLUGINS_DIR "${QT_ROOT_DIR}/plugins")

foreach(qt_dll IN ITEMS
        Qt5Core.dll Qt5Gui.dll Qt5Network.dll Qt5Svg.dll Qt5Widgets.dll
        Qt5WinExtras.dll libEGL.dll libGLESv2.dll)
    if (EXISTS "${QT_BIN_DIR}/${qt_dll}")
        file(REMOVE "${OUTPUT_DIR}/${qt_dll}")
        file(COPY "${QT_BIN_DIR}/${qt_dll}" DESTINATION "${OUTPUT_DIR}")
    else()
        message(FATAL_ERROR "Required Qt runtime not found: ${QT_BIN_DIR}/${qt_dll}")
    endif()
endforeach()

foreach(plugin_dir IN ITEMS bearer iconengines imageformats platforms styles)
    file(REMOVE_RECURSE "${OUTPUT_DIR}/${plugin_dir}")
endforeach()

foreach(plugin_entry IN ITEMS
        "bearer/qgenericbearer.dll"
        "platforms/qwindows.dll"
        "styles/qwindowsvistastyle.dll"
        "iconengines/qsvgicon.dll"
        "imageformats/qgif.dll"
        "imageformats/qicns.dll"
        "imageformats/qico.dll"
        "imageformats/qjpeg.dll"
        "imageformats/qsvg.dll"
        "imageformats/qtga.dll"
        "imageformats/qtiff.dll"
        "imageformats/qwbmp.dll"
        "imageformats/qwebp.dll")
    get_filename_component(plugin_dir "${plugin_entry}" DIRECTORY)
    get_filename_component(plugin_name "${plugin_entry}" NAME)
    if (EXISTS "${QT_PLUGINS_DIR}/${plugin_entry}")
        file(MAKE_DIRECTORY "${OUTPUT_DIR}/${plugin_dir}")
        file(COPY "${QT_PLUGINS_DIR}/${plugin_entry}" DESTINATION "${OUTPUT_DIR}/${plugin_dir}")
    else()
        message(WARNING "Optional Qt plugin not found: ${QT_PLUGINS_DIR}/${plugin_entry}")
    endif()
endforeach()

foreach(stale_qt_dll IN ITEMS icuin58.dll icuuc58.dll icudt58.dll)
    file(REMOVE "${OUTPUT_DIR}/${stale_qt_dll}")
endforeach()

if (DEFINED OPENSSL_ROOT_HINT AND OPENSSL_ROOT_HINT)
    set(OPENSSL_BIN_DIR "${OPENSSL_ROOT_HINT}/bin")
endif()

foreach(ssl_dll IN ITEMS libssl-3-x64.dll libcrypto-3-x64.dll)
    if (DEFINED OPENSSL_BIN_DIR AND EXISTS "${OPENSSL_BIN_DIR}/${ssl_dll}")
        file(COPY "${OPENSSL_BIN_DIR}/${ssl_dll}" DESTINATION "${OUTPUT_DIR}")
    else()
        message(FATAL_ERROR "Required OpenSSL runtime not found: ${ssl_dll}")
    endif()
endforeach()

set(ZLIB_DEST "${OUTPUT_DIR}/zlib.dll")
if (NOT EXISTS "${ZLIB_DEST}")
    set(zlib_search_script [=[
$roots = @()
if ($env:ProgramFiles) { $roots += $env:ProgramFiles }
$programFilesX86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
if ($programFilesX86) { $roots += $programFilesX86 }
$candidates = foreach ($root in $roots) {
    Get-ChildItem -Path $root -Filter zlib.dll -Recurse -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty FullName
}
$preferred = $candidates | Where-Object {
    $_ -like '*MySQL Workbench*' -or $_ -like '*Nsight*'
} | Select-Object -First 1
if (-not $preferred) {
    $preferred = $candidates | Select-Object -First 1
}
if ($preferred) {
    [Console]::Out.Write($preferred)
}
]=])

    execute_process(
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command "${zlib_search_script}"
        OUTPUT_VARIABLE zlib_source
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE zlib_search_result
    )

    if (zlib_search_result EQUAL 0 AND EXISTS "${zlib_source}")
        file(COPY "${zlib_source}" DESTINATION "${OUTPUT_DIR}")
    else()
        message(FATAL_ERROR "zlib.dll was not found; barrier.exe would miss a Qt5Network dependency")
    endif()
endif()

foreach(legacy_dll IN ITEMS libssl-1_1-x64.dll libcrypto-1_1-x64.dll)
    if (EXISTS "${OUTPUT_DIR}/${legacy_dll}")
        file(REMOVE "${OUTPUT_DIR}/${legacy_dll}")
    endif()
endforeach()
