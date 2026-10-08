cmake_minimum_required(VERSION 3.28)
project(RPCS3_UWP LANGUAGES CXX)

if(NOT MSVC OR NOT CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
    message(FATAL_ERROR "Configure RPCS3-UWP with MSVC and CMAKE_SYSTEM_NAME=WindowsStore")
endif()
set(RPCS3_CORE_DLL "" CACHE FILEPATH "Built rpcs3-core.dll to package")
set(RPCS3_CORE_IMPLIB "" CACHE FILEPATH "Matching rpcs3-core.lib import library")
option(RPCS3_HOST_WITH_CORE "Link and package the RPCS3 core" ON)
option(RPCS3_HOST_MESA "Package Mesa Gallium D3D12 for the OpenGL renderer" OFF)
option(RPCS3_HOST_DZN "Package Mesa Dozen for the Vulkan renderer" OFF)
set(RPCS3_DZN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../build-uwp-msvc/mesa-dozen-port" CACHE PATH "Built UWP Mesa Dozen tree")
set(RPCS3_MESA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../build-uwp-msvc/mesa" CACHE PATH "Built UWP Mesa tree")
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
    if(RPCS3_HOST_DZN)
        foreach(dzn_dll IN ITEMS vulkan_dzn.dll dxil.dll)
            set(dzn_path "${RPCS3_DZN_ROOT}/src/microsoft/vulkan/${dzn_dll}")
            if(NOT EXISTS "${dzn_path}")
                message(FATAL_ERROR "Build Mesa Dozen first: ${dzn_path}")
            endif()
            # DXIL is shared with Gallium when both renderers are packaged.
            if(dzn_dll STREQUAL "dxil.dll" AND RPCS3_HOST_MESA)
                continue()
            endif()
            set_source_files_properties("${dzn_path}" TARGET_DIRECTORY RPCS3UwpApp PROPERTIES
                VS_DEPLOYMENT_CONTENT 1 VS_DEPLOYMENT_LOCATION "."
                VS_SETTINGS "DeploymentContent=true;DeploymentLocation=.;Link=${dzn_dll}")
            target_sources(RPCS3UwpApp PRIVATE "${dzn_path}")
            add_custom_command(TARGET rpcs3_core_deploy POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dzn_path}" "$<TARGET_FILE_DIR:RPCS3UwpApp>" VERBATIM)
        endforeach()
    endif()
    if(RPCS3_HOST_MESA)
        foreach(mesa_dll IN ITEMS src/gallium/targets/libgl-gdi/opengl32.dll src/gallium/targets/wgl/gallium_wgl.dll src/gallium/targets/libgl-gdi/dxil.dll)
            set(mesa_path "${RPCS3_MESA_ROOT}/${mesa_dll}")
            if(NOT EXISTS "${mesa_path}")
                message(FATAL_ERROR "Build Mesa first: ${mesa_path}")
            endif()
            get_filename_component(mesa_name "${mesa_path}" NAME)
            set_source_files_properties("${mesa_path}" TARGET_DIRECTORY RPCS3UwpApp PROPERTIES
                VS_DEPLOYMENT_CONTENT 1 VS_DEPLOYMENT_LOCATION "."
                VS_SETTINGS "DeploymentContent=true;DeploymentLocation=.;Link=${mesa_name}")
            target_sources(RPCS3UwpApp PRIVATE "${mesa_path}")
            add_custom_command(TARGET rpcs3_core_deploy POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${mesa_path}" "$<TARGET_FILE_DIR:RPCS3UwpApp>"
                VERBATIM)
        endforeach()
    endif()
endif()
