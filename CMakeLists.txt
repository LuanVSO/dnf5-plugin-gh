cmake_minimum_required(VERSION 3.16)
project(dnf5-plugin-gh VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(PkgConfig REQUIRED)
find_package(nlohmann_json 3 REQUIRED)
pkg_check_modules(LIBDNF5 REQUIRED libdnf5 libdnf5-cli)
pkg_check_modules(CURL REQUIRED libcurl)

include(CTest)
include(GNUInstallDirs)

add_library(ghcore STATIC
    src/assets.cpp
    src/github.cpp
    src/refresh.cpp
    src/store.cpp)
target_include_directories(ghcore PUBLIC src)
target_link_libraries(ghcore PUBLIC nlohmann_json::nlohmann_json ${CURL_LIBRARIES})
target_include_directories(ghcore PUBLIC ${CURL_INCLUDE_DIRS})

add_library(gh_cmd_plugin MODULE src/plugin.cpp)
set_target_properties(gh_cmd_plugin PROPERTIES PREFIX "")
target_compile_definitions(gh_cmd_plugin PRIVATE GETTEXT_DOMAIN="dnf5-plugin-gh")
target_include_directories(gh_cmd_plugin PRIVATE ${LIBDNF5_INCLUDE_DIRS} /usr/include/dnf5)
target_link_libraries(gh_cmd_plugin PRIVATE ghcore ${LIBDNF5_LIBRARIES})

add_library(gh MODULE src/libdnf_plugin.cpp)
set_target_properties(gh PROPERTIES PREFIX "")
target_include_directories(gh PRIVATE ${LIBDNF5_INCLUDE_DIRS})
target_link_libraries(gh PRIVATE ghcore ${LIBDNF5_LIBRARIES})

set(DNF5_PLUGIN_DIR "${CMAKE_INSTALL_FULL_LIBDIR}/dnf5/plugins" CACHE PATH "dnf5 plugin directory")
set(LIBDNF5_PLUGIN_DIR "${CMAKE_INSTALL_FULL_LIBDIR}/libdnf5/plugins" CACHE PATH "libdnf5 plugin directory")
set(LIBDNF5_PLUGIN_CONFIG_DIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}/dnf/libdnf5-plugins" CACHE PATH "libdnf5 plugin configuration directory")
install(TARGETS gh_cmd_plugin LIBRARY DESTINATION ${DNF5_PLUGIN_DIR})
install(TARGETS gh LIBRARY DESTINATION ${LIBDNF5_PLUGIN_DIR})
install(FILES gh.conf DESTINATION ${LIBDNF5_PLUGIN_CONFIG_DIR})

if(BUILD_TESTING)
    add_executable(test_core tests/test_core.cpp)
    target_link_libraries(test_core PRIVATE ghcore)
    add_test(NAME core COMMAND test_core)
endif()
