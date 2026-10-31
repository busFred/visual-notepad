# Platform plugin for the tests. offscreen has no fonts on Windows, where CI
# uses the real "windows" plugin instead (-DVN_TEST_QPA=windows).
set(VN_TEST_QPA offscreen CACHE STRING "QPA platform the tests run on")

find_package(Qt6 REQUIRED COMPONENTS Test)

qt_add_executable(test_mainwindow test_mainwindow.cpp)
target_link_libraries(test_mainwindow PRIVATE vn_app Qt6::Test)
target_compile_definitions(test_mainwindow PRIVATE VN_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME mainwindow COMMAND test_mainwindow)
set_tests_properties(mainwindow PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${VN_TEST_QPA}")

add_test(NAME core COMMAND cargo test --manifest-path ${PROJECT_SOURCE_DIR}/core/Cargo.toml
                           --target-dir ${CMAKE_BINARY_DIR}/cargo-test)
# The test compiles the crate and its dependencies first (optimised even in
# Debug), which outlasts CI's per-test --timeout on small runners.
set_tests_properties(core PROPERTIES TIMEOUT 600)

qt_add_executable(test_textview test_textview.cpp)
target_link_libraries(test_textview PRIVATE vn_app Qt6::Test)
add_test(NAME textview COMMAND test_textview)
set_tests_properties(textview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${VN_TEST_QPA}")

qt_add_executable(test_find test_find.cpp)
target_link_libraries(test_find PRIVATE vn_app Qt6::Test)
add_test(NAME find COMMAND test_find)
set_tests_properties(find PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${VN_TEST_QPA}")

qt_add_executable(test_preferences test_preferences.cpp)
target_link_libraries(test_preferences PRIVATE vn_app Qt6::Test)
add_test(NAME preferences COMMAND test_preferences)
set_tests_properties(preferences PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${VN_TEST_QPA}")

qt_add_executable(test_visualview test_visualview.cpp)
target_link_libraries(test_visualview PRIVATE vn_app Qt6::Test)
target_compile_definitions(test_visualview PRIVATE VN_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME visualview COMMAND test_visualview)
set_tests_properties(visualview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${VN_TEST_QPA}")

qt_add_executable(test_remote_images test_remote_images.cpp)
target_link_libraries(test_remote_images PRIVATE vn_app Qt6::Test)
add_test(NAME remote_images COMMAND test_remote_images)
set_tests_properties(remote_images PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${VN_TEST_QPA}")

qt_add_executable(test_math test_math.cpp)
target_link_libraries(test_math PRIVATE vn_app Qt6::Test)
add_test(NAME math COMMAND test_math)
set_tests_properties(math PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${VN_TEST_QPA}")

qt_add_executable(test_files test_files.cpp)
target_link_libraries(test_files PRIVATE vn_app Qt6::Test)
add_test(NAME files COMMAND test_files)
set_tests_properties(files PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${VN_TEST_QPA}")

qt_add_executable(test_highlighting test_highlighting.cpp)
target_link_libraries(test_highlighting PRIVATE vn_app Qt6::Test)
add_test(NAME highlighting COMMAND test_highlighting)
set_tests_properties(highlighting PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${VN_TEST_QPA}")

qt_add_executable(test_open test_open.cpp)
target_link_libraries(test_open PRIVATE vn_app Qt6::Test)
add_test(NAME open COMMAND test_open)
set_tests_properties(open PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${VN_TEST_QPA}")

qt_add_executable(test_session test_session.cpp)
target_link_libraries(test_session PRIVATE vn_app Qt6::Test)
add_test(NAME session COMMAND test_session)
set_tests_properties(session PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${VN_TEST_QPA}")

qt_add_executable(test_groups test_groups.cpp)
target_link_libraries(test_groups PRIVATE vn_app Qt6::Test)
add_test(NAME groups COMMAND test_groups)
set_tests_properties(groups PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${VN_TEST_QPA}")

qt_add_executable(test_markdown_editing test_markdown_editing.cpp)
target_link_libraries(test_markdown_editing PRIVATE vn_app Qt6::Test)
add_test(NAME markdown_editing COMMAND test_markdown_editing)
set_tests_properties(markdown_editing PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${VN_TEST_QPA}")

qt_add_executable(test_indentation test_indentation.cpp)
target_link_libraries(test_indentation PRIVATE vn_app Qt6::Test)
add_test(NAME indentation COMMAND test_indentation)
set_tests_properties(indentation PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${VN_TEST_QPA}")

qt_add_executable(test_theme test_theme.cpp)
target_link_libraries(test_theme PRIVATE vn_app Qt6::Test)
add_test(NAME theme COMMAND test_theme)
set_tests_properties(theme PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${VN_TEST_QPA}")

qt_add_executable(perf_probe perf_probe.cpp)
target_link_libraries(perf_probe PRIVATE vn_app Qt6::Test)
target_link_options(perf_probe PRIVATE -rdynamic)

qt_add_executable(screenshot_tour screenshot_tour.cpp)
target_link_libraries(screenshot_tour PRIVATE vn_app Qt6::Test)

qt_add_executable(test_formatting test_formatting.cpp)
target_link_libraries(test_formatting PRIVATE vn_app Qt6::Test)
add_test(NAME formatting COMMAND test_formatting)
set_tests_properties(formatting PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${VN_TEST_QPA}")

qt_add_executable(test_shared_docs test_shared_docs.cpp)
target_link_libraries(test_shared_docs PRIVATE vn_app Qt6::Test)
add_test(NAME shared_docs COMMAND test_shared_docs)
set_tests_properties(shared_docs PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${VN_TEST_QPA}")

qt_add_executable(test_editor_grid test_editor_grid.cpp)
target_link_libraries(test_editor_grid PRIVATE vn_app Qt6::Test)
add_test(NAME editor_grid COMMAND test_editor_grid)
set_tests_properties(editor_grid PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${VN_TEST_QPA}")

qt_add_executable(test_links test_links.cpp)
target_link_libraries(test_links PRIVATE vn_app Qt6::Test)
add_test(NAME links COMMAND test_links)
set_tests_properties(links PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${VN_TEST_QPA}")
