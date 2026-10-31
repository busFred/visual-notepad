cmake_minimum_required(VERSION 3.25)

project(visnotepad VERSION 0.0.6 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.10 REQUIRED COMPONENTS Widgets Gui)
find_package(KF6SyntaxHighlighting 6.31 REQUIRED)

include(FetchContent)
FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.6.1
)
FetchContent_MakeAvailable(Corrosion)

include(cmake/MicroTeX.cmake)

corrosion_import_crate(MANIFEST_PATH core/Cargo.toml CRATES vn-core CRATE_TYPES staticlib)
corrosion_add_cxxbridge(vn_core_bridge CRATE vn_core FILES lib.rs)

# Everything except main() lives in a static library so tests can link it.
# Encoding guessing is optional: without ICU files that are not UTF-8 open
# in the legacy encoding.
find_package(ICU COMPONENTS uc i18n)

add_library(vn_app STATIC
    src/Commands.cpp
    src/Commands.h
    src/CustomizeColorsDialog.cpp
    src/CustomizeColorsDialog.h
    src/DocumentTab.cpp
    src/DocumentTab.h
    src/Encodings.cpp
    src/Encodings.h
    src/EditorGroup.cpp
    src/EditorGroup.h
    src/EvenSplitter.cpp
    src/EvenSplitter.h
    src/MainWindow.cpp
    src/MainWindow.h
    src/PreferencesDialog.cpp
    src/PreferencesDialog.h
    src/Session.cpp
    src/Session.h
    src/Settings.cpp
    src/Settings.h
    src/TextFile.cpp
    src/TextFile.h
    src/Theme.cpp
    src/DesktopSettings.cpp
    src/Theme.h
    src/WelcomeView.cpp
    src/WelcomeView.h
    src/editor/DocumentLayout.cpp
    src/editor/DocumentLayout.h
    src/editor/FindBar.cpp
    src/editor/Indentation.cpp
    src/editor/Indentation.h
    src/editor/FindBar.h
    src/editor/Search.cpp
    src/editor/Search.h
    src/editor/TextView.cpp
    src/editor/TextViewFormat.cpp
    src/editor/TextView.h
    src/markdown/BlockRenderer.cpp
    src/markdown/BlockRenderer.h
    src/markdown/ImageViewer.cpp
    src/markdown/ImageViewer.h
    src/markdown/MarkdownEditing.cpp
    src/markdown/MarkdownEditing.h
    src/markdown/MarkdownModel.cpp
    src/markdown/MarkdownModel.h
    src/markdown/MathRenderer.cpp
    src/markdown/MathRenderer.h
    src/markdown/VisualView.cpp
    src/markdown/VisualView.h
)
target_include_directories(vn_app PUBLIC src)

file(GLOB vn_icons RELATIVE ${PROJECT_SOURCE_DIR} CONFIGURE_DEPENDS resources/icons/*.svg)
qt_add_resources(vn_app vn_resources
    PREFIX /vn
    BASE resources
    FILES resources/themes/light-grey-warm.theme resources/app/visnotepad.svg ${vn_icons}
        resources/fonts/Selawik-Regular.ttf resources/fonts/Selawik-Semibold.ttf resources/fonts/Selawik-Bold.ttf
        resources/fonts/Inconsolata-SemiExpanded.ttf resources/fonts/Inconsolata-SemiExpandedBold.ttf
)
target_compile_definitions(vn_app PUBLIC VN_VERSION="${PROJECT_VERSION}")
target_compile_definitions(vn_app PRIVATE VN_MICROTEX_RES="${VN_MICROTEX_RES_BUILD}")
target_link_libraries(vn_app PUBLIC
    Qt6::Widgets
    KF6::SyntaxHighlighting
    vn_core_bridge
    microtex
)

if(ICU_FOUND)
    target_compile_definitions(vn_app PUBLIC VN_HAVE_ICU)
    target_link_libraries(vn_app PUBLIC ICU::i18n ICU::uc)
endif()

if(WIN32)
    enable_language(RC)
    set(VN_WIN_RC packaging/windows/visnotepad.rc)
endif()
qt_add_executable(visnotepad WIN32 src/main.cpp ${VN_WIN_RC})
target_link_libraries(visnotepad PRIVATE vn_app)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS visnotepad RUNTIME DESTINATION bin)
install(DIRECTORY ${VN_MICROTEX_RES_BUILD}/ DESTINATION share/visnotepad/microtex/res)
