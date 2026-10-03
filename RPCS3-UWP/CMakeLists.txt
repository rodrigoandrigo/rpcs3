cmake_minimum_required(VERSION 3.28)
project(RPCS3_UWP LANGUAGES CXX)

if(NOT MSVC OR NOT CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
    message(FATAL_ERROR "Configure RPCS3-UWP with MSVC and CMAKE_SYSTEM_NAME=WindowsStore")
endif()
set(RPCS3_CORE_DLL "" CACHE FILEPATH "Built rpcs3-core.dll to package")
set(RPCS3_CORE_IMPLIB "" CACHE FILEPATH "Matching rpcs3-core.lib import library")
option(RPCS3_HOST_WITH_CORE "Link and package the RPCS3 core" ON)
add_subdirectory(Frontend)
set_target_properties(RPCS3UwpApp PROPERTIES OUTPUT_NAME "RPCS3-UWP")
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT RPCS3UwpApp)

if(RPCS3_HOST_WITH_CORE)
    if(NOT EXISTS "${RPCS3_CORE_DLL}" OR NOT EXISTS "${RPCS3_CORE_IMPLIB}")
        message(FATAL_ERROR "Build the core DLL first, then provide RPCS3_CORE_DLL and RPCS3_CORE_IMPLIB")
    endif()
    add_library(RPCS3::Core SHARED IMPORTED)
    set_target_properties(RPCS3::Core PROPERTIES
        IMPORTED_LOCATION "${RPCS3_CORE_DLL}"
        IMPORTED_IMPLIB "${RPCS3_CORE_IMPLIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../rpcs3/Embedded")
    target_link_libraries(RPCS3UwpFrontendHost PUBLIC RPCS3::Core)
    target_compile_definitions(RPCS3UwpFrontendHost PUBLIC RPCS3_HOST_WITH_CORE=1)
    set_source_files_properties("${RPCS3_CORE_DLL}" TARGET_DIRECTORY RPCS3UwpApp PROPERTIES
        VS_DEPLOYMENT_CONTENT 1 VS_DEPLOYMENT_LOCATION "."
        VS_SETTINGS "DeploymentContent=true;DeploymentLocation=.;Link=rpcs3-core.dll")
    target_sources(RPCS3UwpApp PRIVATE "${RPCS3_CORE_DLL}")
    # The app target is defined in a child directory; a cross-directory
    # TARGET/POST_BUILD command is invalid. Stage before app packaging instead.
    add_custom_target(rpcs3_core_deploy
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:RPCS3UwpApp>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RPCS3_CORE_DLL}"
            "$<TARGET_FILE_DIR:RPCS3UwpApp>/rpcs3-core.dll"
        DEPENDS "${RPCS3_CORE_DLL}"
        VERBATIM)
    add_dependencies(RPCS3UwpApp rpcs3_core_deploy)
endif()
