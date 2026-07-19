cmake_minimum_required(VERSION 3.21)
project(ShaderPlayer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
# Small UI assets are compiled into the exe, so there is no icons/ to deploy or resolve.
set(CMAKE_AUTORCC ON)

set(CMAKE_PREFIX_PATH "C:/Qt/6.8.3/msvc2022_64" CACHE STRING "Qt install prefix")
find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets)

# The Hooks panel (proxy-DLL shader injection into other games). Off for the Store build, which
# cannot ship injectors, exactly like SHADERPLAYER_EMULATION.
option(SHADERPLAYER_HOOKS "Build the Hooks panel (shader injection into other games)" ON)

set(EXTERN "${CMAKE_SOURCE_DIR}/extern")

set(EXECUTABLE_NAME "ShaderPlayer")

add_executable(${EXECUTABLE_NAME} WIN32
    src/appdata.cpp
    src/main.cpp
    src/mainwindow.cpp
    src/playlistpanel.cpp
    src/shaderpanel.cpp
    src/shaderparamspanel.cpp
    src/videowidget.cpp
    resources.qrc
    src/ShaderPlayer.rc
)

target_include_directories(${EXECUTABLE_NAME} PRIVATE
    "${EXTERN}/mpv/include"
    "${EXTERN}/librashader"
    "${CMAKE_SOURCE_DIR}"
)

target_compile_definitions(${EXECUTABLE_NAME} PRIVATE
    # librashader's GL entry points are gated behind this define.
    LIBRA_RUNTIME_OPENGL
    # Dev fallback for the shader library; an installed build uses ./shaders next to the exe.
    SHADERPLAYER_SHADER_DIR="${EXTERN}/slang-shaders"
    # Dev fallback for the bundled factory defaults; an installed build uses ./defaults.ini.
    SHADERPLAYER_DEFAULTS="${CMAKE_SOURCE_DIR}/defaults.ini"
)

if(SHADERPLAYER_HOOKS)
    target_sources(${EXECUTABLE_NAME} PRIVATE src/hookspanel.cpp src/hookworker.cpp)
    target_compile_definitions(${EXECUTABLE_NAME} PRIVATE
        SHADERPLAYER_HOOKS
        # The hook worker runs librashader's D3D11 backend; the video path is still GL.
        LIBRA_RUNTIME_D3D11
        # Dev fallback for the hook DLLs + injection helper; installed builds use the exe's own folder.
        SHADERPLAYER_HOOK_DIR="${CMAKE_SOURCE_DIR}/hooks/inject/bin"
    )
    # winmm: timeBeginPeriod, so the capture loop's polling is not rounded up to a 60Hz-ish tick.
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE d3d11 dxgi winmm)
endif()

target_link_libraries(${EXECUTABLE_NAME} PRIVATE
    Qt6::Widgets
    Qt6::OpenGLWidgets
    "${EXTERN}/mpv/libmpv.lib"
    "${EXTERN}/librashader/librashader.dll.lib"
    opengl32
    dbghelp
)

# Both DLLs and the Qt runtime need to sit next to the .exe to run from the build dir.
# The release zip ships the DLL as librashader.dll, but its import lib was built from the
# librashader_capi crate and binds to librashader_capi.dll, so it must be staged under that name.
add_custom_command(TARGET ${EXECUTABLE_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/defaults.ini" "$<TARGET_FILE_DIR:${EXECUTABLE_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${EXTERN}/mpv/libmpv-2.dll" "$<TARGET_FILE_DIR:${EXECUTABLE_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${EXTERN}/librashader/librashader.dll"
        "$<TARGET_FILE_DIR:${EXECUTABLE_NAME}>/librashader_capi.dll"
    COMMAND "${CMAKE_PREFIX_PATH}/bin/windeployqt.exe" --no-translations --no-compiler-runtime
        "$<TARGET_FILE:${EXECUTABLE_NAME}>"
    VERBATIM
)
