# MicroTeX (cLaTeXMath line, MIT) built as a small static library with its Qt
# backend. Upstream's CMakeLists is not used: it hard-codes global flags and
# builds sample executables. tinyxml2 (zlib license) is compiled in as well.
include(FetchContent)

# SOURCE_SUBDIR points at a missing directory so MakeAvailable only downloads.
FetchContent_Declare(
    microtex_src
    GIT_REPOSITORY https://github.com/NanoMichael/MicroTeX.git
    GIT_TAG 0e3707f6dafebb121d98b53c64364d16fefe481d
    SOURCE_SUBDIR no-cmake
)
FetchContent_Declare(
    tinyxml2_src
    GIT_REPOSITORY https://github.com/leethomason/tinyxml2.git
    GIT_TAG 10.0.0
    SOURCE_SUBDIR no-cmake
)
FetchContent_MakeAvailable(microtex_src tinyxml2_src)

set(_mt ${microtex_src_SOURCE_DIR})
file(GLOB_RECURSE _mt_sources CONFIGURE_DEPENDS
    ${_mt}/src/atom/*.cpp
    ${_mt}/src/box/*.cpp
    ${_mt}/src/core/*.cpp
    ${_mt}/src/fonts/*.cpp
    ${_mt}/src/utils/*.cpp
    ${_mt}/src/res/*.cpp
)
add_library(microtex STATIC
    ${_mt_sources}
    ${_mt}/src/latex.cpp
    ${_mt}/src/render.cpp
    ${_mt}/src/platform/qt/graphic_qt.cpp
    ${tinyxml2_src_SOURCE_DIR}/tinyxml2.cpp
)
target_include_directories(microtex SYSTEM PUBLIC ${_mt}/src)
target_include_directories(microtex SYSTEM PRIVATE ${tinyxml2_src_SOURCE_DIR})
target_compile_definitions(microtex PUBLIC BUILD_QT)
target_compile_features(microtex PUBLIC cxx_std_17)
target_link_libraries(microtex PRIVATE Qt6::Gui)
if(MSVC)
    target_compile_options(microtex PRIVATE /w /utf-8)
    target_compile_definitions(microtex PUBLIC _HAS_STD_BYTE=0)
else()
    target_compile_options(microtex PRIVATE -w)
endif()
set_target_properties(microtex PROPERTIES AUTOMOC OFF POSITION_INDEPENDENT_CODE ON)

# Runtime resources (fonts and XML). Copied into the build tree for development
# runs and tests; installed to share/visnotepad/microtex/res.
set(VN_MICROTEX_RES_BUILD ${CMAKE_BINARY_DIR}/microtex/res)
# res/greek and res/cyrillic are GPL-3.0 and are left out. They only serve
# literal Greek/Cyrillic letters inside \text{}, which then use a system font
# (\alpha and friends come from the Computer Modern fonts and still work).
add_custom_target(microtex_res ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${VN_MICROTEX_RES_BUILD}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${_mt}/res ${VN_MICROTEX_RES_BUILD}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${VN_MICROTEX_RES_BUILD}/greek ${VN_MICROTEX_RES_BUILD}/cyrillic
    COMMENT "Copying MicroTeX resources (without the GPL Greek/Cyrillic packages)"
)
add_dependencies(microtex microtex_res)
set(VN_MICROTEX_RES_SOURCE ${_mt}/res)
