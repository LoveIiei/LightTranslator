cmake_minimum_required(VERSION 3.25)
project(LightTranslate VERSION 1.0.0 LANGUAGES CXX RC)

if(NOT MSVC)
    message(FATAL_ERROR "LightTranslate targets Windows 11 and builds with MSVC only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)                     # /GL + /LTCG
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # static CRT, no VC++ redist

option(LT_BUILD_TESTS "Build unit tests" ON)

# ---- Third-party (header-only, pinned tags) --------------------------------
# SOURCE_SUBDIR points at a directory without a CMakeLists.txt on purpose: we only
# need the headers, not the upstream build scripts/tests.
include(FetchContent)
FetchContent_Declare(wil
    GIT_REPOSITORY https://github.com/microsoft/wil.git
    GIT_TAG        v1.0.250325.1
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _headers_only)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _headers_only)
FetchContent_MakeAvailable(wil nlohmann_json)

# ---- Shared build settings --------------------------------------------------
add_library(lt_options INTERFACE)
target_compile_definitions(lt_options INTERFACE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_compile_options(lt_options INTERFACE
    /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus /guard:cf
    $<$<CONFIG:Release>:/O2 /Gy /Gw>)
target_link_options(lt_options INTERFACE
    /guard:cf /CETCOMPAT /DYNAMICBASE /HIGHENTROPYVA
    $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)
target_include_directories(lt_options INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/res)
target_include_directories(lt_options SYSTEM INTERFACE
    ${wil_SOURCE_DIR}/include
    ${nlohmann_json_SOURCE_DIR}/single_include)

# Providers are pure (no I/O): the same sources are compiled into the app and the tests.
set(LT_PROVIDER_SOURCES
    src/core/Encoding.cpp
    src/providers/ProviderRegistry.cpp
    src/providers/DeepLProvider.cpp
    src/providers/GoogleProvider.cpp)

# ---- Application ------------------------------------------------------------
add_executable(translator WIN32
    src/main.cpp
    src/app/App.cpp
    src/app/Settings.cpp
    src/core/HotkeyManager.cpp
    src/core/ClipboardBackup.cpp
    src/core/TextCapture.cpp
    src/core/TranslatorClient.cpp
    src/core/SecretStore.cpp
    ${LT_PROVIDER_SOURCES}
    src/ui/TrayIcon.cpp
    src/ui/Theme.cpp
    src/ui/PopupRenderer.cpp
    src/ui/PopupWindow.cpp
    src/ui/SettingsDialog.cpp
    res/app.rc
    res/app.manifest)   # merged into the embedded manifest by the linker
set_target_properties(translator PROPERTIES OUTPUT_NAME "LightTranslate")
target_link_libraries(translator PRIVATE lt_options
    user32 gdi32 shell32 ole32 oleaut32 uuid comctl32 dwmapi winhttp advapi32
    d3d11 dxgi d2d1 dwrite dcomp
    uiautomationcore)   # provider-side UIA (UiaReturnRawElementProvider, notifications)

# ---- Tests ------------------------------------------------------------------
if(LT_BUILD_TESTS)
    enable_testing()
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG        v2.4.11
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  _headers_only)
    FetchContent_MakeAvailable(doctest)

    add_executable(translator_tests
        tests/test_main.cpp
        tests/test_encoding.cpp
        tests/test_lru.cpp
        tests/test_providers.cpp
        ${LT_PROVIDER_SOURCES})
    target_link_libraries(translator_tests PRIVATE lt_options)
    target_include_directories(translator_tests SYSTEM PRIVATE ${doctest_SOURCE_DIR})
    target_compile_definitions(translator_tests PRIVATE
        LT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME unit COMMAND translator_tests)
endif()
