find_package(Qt6 REQUIRED COMPONENTS Test)

qt_add_executable(test_mainwindow test_mainwindow.cpp)
target_link_libraries(test_mainwindow PRIVATE vn_app Qt6::Test)
add_test(NAME mainwindow COMMAND test_mainwindow)
set_tests_properties(mainwindow PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_test(NAME core COMMAND cargo test --manifest-path ${PROJECT_SOURCE_DIR}/core/Cargo.toml
                           --target-dir ${CMAKE_BINARY_DIR}/cargo-test)
