cmake_minimum_required(VERSION 3.12)
project(jtag_transport_tests LANGUAGES CXX)
enable_testing()
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
add_executable(usbblaster_test usbblaster_test.cpp
    ../src/jtag.cpp ../src/usbBlaster.cpp ../src/display.cpp)
target_compile_features(usbblaster_test PRIVATE cxx_std_17)
target_compile_definitions(usbblaster_test PRIVATE
    ENABLE_USBBLASTER ENABLE_USB_BLASTERII BLASTERII_DIR="")
target_include_directories(usbblaster_test PRIVATE ../src ${LIBUSB_INCLUDE_DIRS})
target_link_libraries(usbblaster_test PRIVATE ${LIBUSB_LDFLAGS})
add_test(NAME usbblaster COMMAND usbblaster_test)

add_executable(fx2_read_test fx2_read_test.cpp ../src/fx2_ll.cpp
    ../src/ihexParser.cpp ../src/configBitstreamParser.cpp
    ../src/progressBar.cpp ../src/display.cpp)
target_compile_features(fx2_read_test PRIVATE cxx_std_17)
target_include_directories(fx2_read_test PRIVATE ../src ${LIBUSB_INCLUDE_DIRS})
target_link_libraries(fx2_read_test PRIVATE ${LIBUSB_LDFLAGS})
add_test(NAME fx2_read COMMAND fx2_read_test)

if(APPLE)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    set(OPENFPGALOADER_EXECUTABLE "" CACHE FILEPATH "Built CLI for macOS bundle tests")
    if(Python3_Interpreter_FOUND AND OPENFPGALOADER_EXECUTABLE)
        add_test(NAME macos_bundle COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/macos_bundle_test.py ${OPENFPGALOADER_EXECUTABLE})
        set_tests_properties(macos_bundle PROPERTIES
            ENVIRONMENT PYTHONDONTWRITEBYTECODE=1 TIMEOUT 90)
    endif()
endif()
