cmake_minimum_required(VERSION 3.21)
project(ShaderPlayer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
# Small UI assets are compiled into the exe, so there is no icons/ to deploy or resolve.
set(CMAKE_AUTORCC ON)

set(CMAKE_PREFIX_PATH "C:/Qt/6.8.3/msvc2022_64" CACHE STRING "Qt install prefix")
find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets)

set(EXTERN "${CMAKE_SOURCE_DIR}/extern")

add_executable(ShaderPlayer WIN32
    src/appdata.cpp
    src/main.cpp
    src/mainwindow.cpp
    src/playlistpanel.cpp
    src/shaderpanel.cpp
    src/videowidget.cpp
    resources.qrc
)

target_include_directories(ShaderPlayer PRIVATE
    "${EXTERN}/mpv/include"
    "${EXTERN}/librashader"
)

target_compile_definitions(ShaderPlayer PRIVATE
    # librashader's GL entry points are gated behind this define.
    LIBRA_RUNTIME_OPENGL
    # Dev fallback for the shader library; an installed build uses ./shaders next to the exe.
    SHADERPLAYER_SHADER_DIR="${EXTERN}/slang-shaders"
)

target_link_libraries(ShaderPlayer PRIVATE
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
add_custom_command(TARGET ShaderPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${EXTERN}/mpv/libmpv-2.dll" "$<TARGET_FILE_DIR:ShaderPlayer>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${EXTERN}/librashader/librashader.dll"
        "$<TARGET_FILE_DIR:ShaderPlayer>/librashader_capi.dll"
    COMMAND "${CMAKE_PREFIX_PATH}/bin/windeployqt.exe" --no-translations --no-compiler-runtime
        "$<TARGET_FILE:ShaderPlayer>"
    VERBATIM
)
