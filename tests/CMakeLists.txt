find_package(Qt6 REQUIRED COMPONENTS Test)

qt_add_executable(test_mainwindow test_mainwindow.cpp)
target_link_libraries(test_mainwindow PRIVATE vn_app Qt6::Test)
add_test(NAME mainwindow COMMAND test_mainwindow)
set_tests_properties(mainwindow PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_test(NAME core COMMAND cargo test --manifest-path ${PROJECT_SOURCE_DIR}/core/Cargo.toml
                           --target-dir ${CMAKE_BINARY_DIR}/cargo-test)

qt_add_executable(test_textview test_textview.cpp)
target_link_libraries(test_textview PRIVATE vn_app Qt6::Test)
add_test(NAME textview COMMAND test_textview)
set_tests_properties(textview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(test_find test_find.cpp)
target_link_libraries(test_find PRIVATE vn_app Qt6::Test)
add_test(NAME find COMMAND test_find)
set_tests_properties(find PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
