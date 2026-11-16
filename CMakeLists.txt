cmake_minimum_required(VERSION 3.20)
project(CPUSimulator LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Simulator core: no graphics dependencies, unit-testable ----
add_library(simcore STATIC
    src/isa.cpp
    src/assembler.cpp
    src/cache.cpp
    src/cpu.cpp
    src/learn.cpp)
target_include_directories(simcore PUBLIC src)

add_executable(core_tests tests/core_tests.cpp)
target_link_libraries(core_tests PRIVATE simcore)
enable_testing()
add_test(NAME core_tests COMMAND core_tests)

# ---- GUI: Dear ImGui + GLFW + OpenGL, fetched at configure time ----
option(CPUSIM_BUILD_GUI "Build the graphical application" ON)
if(CPUSIM_BUILD_GUI)
    include(FetchContent)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
        GIT_SHALLOW TRUE)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.5
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(glfw imgui)

    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
    target_include_directories(imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(imgui PUBLIC glfw)

    find_package(OpenGL REQUIRED)

    add_executable(cpu_simulator src/main.cpp src/ui.cpp)
    target_link_libraries(cpu_simulator PRIVATE simcore imgui glfw OpenGL::GL)
    if(MSVC)
        # GUI app without a console window, but keep the standard main().
        target_link_options(cpu_simulator PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    endif()
endif()
