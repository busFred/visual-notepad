cmake_minimum_required(VERSION 3.25)

project(visnotepad VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.10 REQUIRED COMPONENTS Widgets)
find_package(KF6SyntaxHighlighting 6.31 REQUIRED)

include(FetchContent)
FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.6.1
)
FetchContent_MakeAvailable(Corrosion)

corrosion_import_crate(MANIFEST_PATH core/Cargo.toml CRATES vn-core CRATE_TYPES staticlib)
corrosion_add_cxxbridge(vn_core_bridge CRATE vn_core FILES lib.rs)

# Everything except main() lives in a static library so tests can link it.
add_library(vn_app STATIC
    src/DocumentTab.cpp
    src/DocumentTab.h
    src/EditorGroup.cpp
    src/EditorGroup.h
    src/MainWindow.cpp
    src/MainWindow.h
    src/Theme.cpp
    src/Theme.h
    src/editor/DocumentLayout.cpp
    src/editor/DocumentLayout.h
    src/editor/FindBar.cpp
    src/editor/FindBar.h
    src/editor/Search.cpp
    src/editor/Search.h
    src/editor/TextView.cpp
    src/editor/TextView.h
)
target_include_directories(vn_app PUBLIC src)

file(GLOB vn_icons RELATIVE ${PROJECT_SOURCE_DIR} CONFIGURE_DEPENDS resources/icons/*.svg)
qt_add_resources(vn_app vn_resources
    PREFIX /vn
    BASE resources
    FILES resources/themes/light-grey-warm.theme ${vn_icons}
)
target_compile_definitions(vn_app PUBLIC VN_VERSION="${PROJECT_VERSION}")
target_link_libraries(vn_app PUBLIC
    Qt6::Widgets
    KF6::SyntaxHighlighting
    vn_core_bridge
)

qt_add_executable(visnotepad WIN32 src/main.cpp)
target_link_libraries(visnotepad PRIVATE vn_app)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS visnotepad RUNTIME DESTINATION bin)
