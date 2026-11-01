cmake_minimum_required(VERSION 3.28)

project(PianoCraft VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
set(CLAP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG 1.2.10
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(clap)

set(PIANOCRAFT_ENGINE_SOURCES
    src/engine/PianoEngine.cpp
    src/engine/Soundboard.cpp
    src/engine/SympatheticBank.cpp
    src/model/InstrumentModel.cpp
)

add_library(pianocraft MODULE
    src/host/ClapHost.cpp
    ${PIANOCRAFT_ENGINE_SOURCES}
)
file(GLOB PIANOCRAFT_PRESETS CONFIGURE_DEPENDS resources/presets/*.pianocraft)
target_sources(pianocraft PRIVATE ${PIANOCRAFT_PRESETS})
set_source_files_properties(${PIANOCRAFT_PRESETS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/Presets)
target_link_libraries(pianocraft PRIVATE clap)
target_include_directories(pianocraft PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(pianocraft PROPERTIES
    PREFIX ""
    OUTPUT_NAME "PianoCraft"
    BUNDLE TRUE
    BUNDLE_EXTENSION "clap"
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist"
    MACOSX_BUNDLE_BUNDLE_NAME "PianoCraft"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.pianocraft.upright"
    MACOSX_BUNDLE_BUNDLE_VERSION "0.1.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

if(APPLE)
    target_link_options(pianocraft PRIVATE "LINKER:-exported_symbol,_clap_entry")
else()
    message(FATAL_ERROR "PianoCraft currently supports only macOS CLAP bundles")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/render.cpp")
    add_executable(pianocraft-render
        tools/render.cpp
        ${PIANOCRAFT_ENGINE_SOURCES}
    )
    target_include_directories(pianocraft-render PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/benchmark.cpp")
    add_executable(pianocraft-benchmark
        tools/benchmark.cpp
        ${PIANOCRAFT_ENGINE_SOURCES}
    )
    target_include_directories(pianocraft-benchmark PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()

enable_testing()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/engine_tests.cpp")
    add_executable(pianocraft-engine-tests
        tests/engine_tests.cpp
        ${PIANOCRAFT_ENGINE_SOURCES}
    )
    target_include_directories(pianocraft-engine-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME pianocraft-engine-tests COMMAND pianocraft-engine-tests)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/state_tests.cpp")
    add_executable(pianocraft-state-tests tests/state_tests.cpp)
    target_link_libraries(pianocraft-state-tests PRIVATE clap)
    add_dependencies(pianocraft-state-tests pianocraft)
    add_test(NAME pianocraft-state-tests COMMAND pianocraft-state-tests $<TARGET_FILE:pianocraft> $<TARGET_BUNDLE_DIR:pianocraft>)
endif()

install(TARGETS pianocraft
    DESTINATION "$ENV{HOME}/Library/Audio/Plug-Ins/CLAP"
)
