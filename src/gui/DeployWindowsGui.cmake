if (NOT WIN32)
    return()
endif()

if (NOT DEFINED TARGET_FILE OR NOT EXISTS "${TARGET_FILE}")
    message(FATAL_ERROR "TARGET_FILE is missing for GUI deployment")
endif()

if (NOT DEFINED OUTPUT_DIR OR NOT IS_DIRECTORY "${OUTPUT_DIR}")
    message(FATAL_ERROR "OUTPUT_DIR is missing for GUI deployment")
endif()

if (NOT DEFINED BUILD_CONFIGURATION OR BUILD_CONFIGURATION STREQUAL "")
    message(FATAL_ERROR "BUILD_CONFIGURATION is missing for GUI deployment")
endif()

string(TOLOWER "${BUILD_CONFIGURATION}" build_configuration_lower)
if (build_configuration_lower STREQUAL "debug")
    set(QT_DEPLOY_ARGUMENT --debug)
    set(QT_RUNTIME_SUFFIX d)
else()
    set(QT_DEPLOY_ARGUMENT --release)
    set(QT_RUNTIME_SUFFIX "")
endif()

if (DEFINED WINDEPLOYQT_EXECUTABLE AND EXISTS "${WINDEPLOYQT_EXECUTABLE}")
    execute_process(
        COMMAND "${WINDEPLOYQT_EXECUTABLE}"
            ${QT_DEPLOY_ARGUMENT}
            --no-translations
            --no-system-d3d-compiler
            --no-opengl-sw
            "${TARGET_FILE}"
        RESULT_VARIABLE deploy_result
    )
    if (NOT deploy_result EQUAL 0)
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

set(qt_runtime_dlls
    "Qt5Core${QT_RUNTIME_SUFFIX}.dll"
    "Qt5Gui${QT_RUNTIME_SUFFIX}.dll"
    "Qt5Network${QT_RUNTIME_SUFFIX}.dll"
    "Qt5Svg${QT_RUNTIME_SUFFIX}.dll"
    "Qt5Widgets${QT_RUNTIME_SUFFIX}.dll"
    "Qt5WinExtras${QT_RUNTIME_SUFFIX}.dll"
    "libEGL${QT_RUNTIME_SUFFIX}.dll"
    "libGLESv2${QT_RUNTIME_SUFFIX}.dll")
foreach(qt_dll IN LISTS qt_runtime_dlls)
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

set(qt_plugin_entries
    "bearer/qgenericbearer${QT_RUNTIME_SUFFIX}.dll"
    "platforms/qwindows${QT_RUNTIME_SUFFIX}.dll"
    "styles/qwindowsvistastyle${QT_RUNTIME_SUFFIX}.dll"
    "iconengines/qsvgicon${QT_RUNTIME_SUFFIX}.dll"
    "imageformats/qgif${QT_RUNTIME_SUFFIX}.dll"
    "imageformats/qicns${QT_RUNTIME_SUFFIX}.dll"
    "imageformats/qico${QT_RUNTIME_SUFFIX}.dll"
    "imageformats/qjpeg${QT_RUNTIME_SUFFIX}.dll"
    "imageformats/qsvg${QT_RUNTIME_SUFFIX}.dll"
    "imageformats/qtga${QT_RUNTIME_SUFFIX}.dll"
    "imageformats/qtiff${QT_RUNTIME_SUFFIX}.dll"
    "imageformats/qwbmp${QT_RUNTIME_SUFFIX}.dll"
    "imageformats/qwebp${QT_RUNTIME_SUFFIX}.dll")
foreach(plugin_entry IN LISTS qt_plugin_entries)
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
        file(REMOVE "${OUTPUT_DIR}/${ssl_dll}")
        file(COPY "${OPENSSL_BIN_DIR}/${ssl_dll}" DESTINATION "${OUTPUT_DIR}")
    else()
        message(FATAL_ERROR "Required OpenSSL runtime not found: ${ssl_dll}")
    endif()
endforeach()

file(REMOVE "${OUTPUT_DIR}/zlib.dll" "${OUTPUT_DIR}/zlib1.dll")

foreach(legacy_dll IN ITEMS libssl-1_1-x64.dll libcrypto-1_1-x64.dll)
    if (EXISTS "${OUTPUT_DIR}/${legacy_dll}")
        file(REMOVE "${OUTPUT_DIR}/${legacy_dll}")
    endif()
endforeach()

if (NOT build_configuration_lower STREQUAL "release")
    return()
endif()

if (NOT DEFINED STAGING_DIR OR STAGING_DIR STREQUAL "")
    if (DEFINED STAGING_REQUIRED AND STAGING_REQUIRED)
        message(FATAL_ERROR "STAGING_DIR is missing for Release deployment")
    endif()
    return()
endif()

get_filename_component(output_dir_absolute "${OUTPUT_DIR}" ABSOLUTE)
get_filename_component(staging_dir_absolute "${STAGING_DIR}" ABSOLUTE)
if (staging_dir_absolute STREQUAL output_dir_absolute OR
    staging_dir_absolute MATCHES "^([A-Za-z]:)?/$")
    message(FATAL_ERROR "unsafe Windows Release staging directory: ${STAGING_DIR}")
endif()

set(release_runtime_files
    weave.exe
    weavec.exe
    weaved.exe
    weaves.exe
    Qt5Core.dll
    Qt5Gui.dll
    Qt5Network.dll
    Qt5Svg.dll
    Qt5Widgets.dll
    Qt5WinExtras.dll
    libEGL.dll
    libGLESv2.dll
    libcrypto-3-x64.dll
    libssl-3-x64.dll
    bearer/qgenericbearer.dll
    iconengines/qsvgicon.dll
    imageformats/qgif.dll
    imageformats/qicns.dll
    imageformats/qico.dll
    imageformats/qjpeg.dll
    imageformats/qsvg.dll
    imageformats/qtga.dll
    imageformats/qtiff.dll
    imageformats/qwbmp.dll
    imageformats/qwebp.dll
    platforms/qwindows.dll
    styles/qwindowsvistastyle.dll)
list(SORT release_runtime_files)

file(REMOVE_RECURSE "${staging_dir_absolute}")
set(staging_payload_dir "${staging_dir_absolute}/payload")
file(MAKE_DIRECTORY "${staging_payload_dir}")
set(release_manifest "")
set(inno_payload_entries "")
foreach(relative_path IN LISTS release_runtime_files)
    if (IS_ABSOLUTE "${relative_path}" OR relative_path MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR "unsafe Windows Release manifest path: ${relative_path}")
    endif()

    set(source_path "${OUTPUT_DIR}/${relative_path}")
    if (NOT EXISTS "${source_path}" OR IS_DIRECTORY "${source_path}")
        message(FATAL_ERROR
                "Required Windows Release runtime is missing: ${source_path}")
    endif()

    get_filename_component(relative_directory "${relative_path}" DIRECTORY)
    if (relative_directory STREQUAL "")
        set(destination_directory "${staging_payload_dir}")
    else()
        set(destination_directory
            "${staging_payload_dir}/${relative_directory}")
        file(MAKE_DIRECTORY "${destination_directory}")
    endif()
    file(COPY "${source_path}" DESTINATION "${destination_directory}")

    file(SHA256 "${source_path}" runtime_sha256)
    string(TOLOWER "${runtime_sha256}" runtime_sha256)
    string(APPEND release_manifest
           "${runtime_sha256}  ${relative_path}\n")

    if (relative_directory STREQUAL "")
        set(inno_destination "{app}")
    else()
        string(REPLACE "/" "\\" inno_relative_directory
                       "${relative_directory}")
        set(inno_destination "{app}\\${inno_relative_directory}")
    endif()
    string(APPEND inno_payload_entries
           "Source: \"{#ReleaseStage}/payload/${relative_path}\"; "
           "DestDir: \"${inno_destination}\"; Flags: ignoreversion\n")
endforeach()

file(WRITE "${staging_dir_absolute}/manifest.sha256" "${release_manifest}")
file(WRITE "${staging_dir_absolute}/payload-files.iss"
     "${inno_payload_entries}")
