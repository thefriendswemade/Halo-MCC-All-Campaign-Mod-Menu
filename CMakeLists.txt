cmake_minimum_required(VERSION 3.20)
project(HaloModMenu LANGUAGES C CXX ASM_MASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT so the DLL has no runtime dependencies of its own.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
set(MINHOOK_DIR ${CMAKE_SOURCE_DIR}/third_party/minhook)
set(MINIZ_DIR ${CMAKE_SOURCE_DIR}/third_party/miniz)  # zlib inflate for Halo 2's compressed .map files

add_library(HaloModMenu SHARED
    src/main.cpp
    src/core/log.cpp
    src/core/patch.cpp
    src/core/proxy.cpp
    src/core/proxy_stubs.asm
    src/core/version.def
    src/mcc/coop.cpp
    src/mcc/mcc.cpp
    src/games/fun.cpp
    src/games/games.cpp
    src/games/halo1/cheats.cpp
    src/games/halo1/halo1.cpp
    src/games/halo1/main_thread.cpp
    src/games/halo1/tags.cpp
    src/games/halo2/ai_upgrade.cpp
    src/games/halo2/cheats.cpp
    src/games/halo2/extras.cpp
    src/games/halo2/fun_extras.cpp
    src/games/halo2/god_extras.cpp
    src/games/halo2/halo2.cpp
    src/games/halo2/main_thread.cpp
    src/games/halo2/tags.cpp
    src/games/halo3/cheats.cpp
    src/games/halo3/ai_upgrade.cpp
    src/games/halo3/bombing_run.cpp
    src/games/halo3/checkpoints.cpp
    src/games/halo3/equipment.cpp
    src/games/halo3/extras.cpp
    src/games/halo3/fun_extras.cpp
    src/games/halo3/teams.cpp
    src/games/halo3/vehicles.cpp
    src/games/halo3/halo3.cpp
    src/games/halo3/main_thread.cpp
    src/games/halo3/spawn.cpp
    src/games/halo3/tags.cpp
    src/games/halo4/cheats.cpp
    src/games/halo4/ai_upgrade.cpp
    src/games/halo4/checkpoints.cpp
    src/games/halo4/equipment.cpp
    src/games/halo4/extras.cpp
    src/games/halo4/fun_extras.cpp
    src/games/halo4/teams.cpp
    src/games/halo4/vehicles.cpp
    src/games/halo4/halo4.cpp
    src/games/halo4/main_thread.cpp
    src/games/halo4/spawn.cpp
    src/games/halo4/tags.cpp
    src/games/odst/cheats.cpp
    src/games/odst/ai_upgrade.cpp
    src/games/odst/checkpoints.cpp
    src/games/odst/equipment.cpp
    src/games/odst/extras.cpp
    src/games/odst/fun_extras.cpp
    src/games/odst/teams.cpp
    src/games/odst/vehicles.cpp
    src/games/odst/odst.cpp
    src/games/odst/main_thread.cpp
    src/games/odst/spawn.cpp
    src/games/odst/tags.cpp
    src/games/reach/boundary_view.cpp
    src/games/reach/cheats.cpp
    src/games/reach/main_thread.cpp
    src/games/reach/reach.cpp
    src/games/reach/tags.cpp
    src/ui/gamepad.cpp
    src/ui/icon_font.cpp
    src/ui/overlay.cpp
    src/ui/settings_file.cpp
    src/ui/menu_config.cpp

    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp

    ${MINHOOK_DIR}/src/buffer.c
    ${MINHOOK_DIR}/src/hook.c
    ${MINHOOK_DIR}/src/trampoline.c
    ${MINHOOK_DIR}/src/hde/hde64.c

    ${MINIZ_DIR}/miniz.c
)

target_include_directories(HaloModMenu PRIVATE
    src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${MINHOOK_DIR}/include
    ${MINIZ_DIR}
)

# We feed ImGui controller input ourselves (ui/gamepad.cpp) with our own button mapping.
target_compile_definitions(HaloModMenu PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    IMGUI_IMPL_WIN32_DISABLE_GAMEPAD MINIZ_NO_ARCHIVE_APIS MINIZ_NO_ARCHIVE_WRITING_APIS MINIZ_NO_TIME)
target_link_libraries(HaloModMenu PRIVATE d3d11 d3dcompiler)

# The game loads this as VERSION.dll from its own folder.
set_target_properties(HaloModMenu PROPERTIES OUTPUT_NAME "version")
