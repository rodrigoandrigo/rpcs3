if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "The embedded UWP core currently requires MSVC on Windows")
endif()

target_compile_definitions(rpcs3_emu PUBLIC RPCS3_UWP=1)
if(RPCS3_UWP_D3D12)
    file(GLOB embedded_d3d12_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Emu/RSX/D3D12/*.cpp")
    # Retired D2D/desktop overlays are not used by the embedded frontend.
    list(FILTER embedded_d3d12_sources EXCLUDE REGEX "D3D12Overlay\\.cpp$")
    target_sources(rpcs3_emu PRIVATE ${embedded_d3d12_sources})
    set_source_files_properties(${embedded_d3d12_sources} PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/3rdparty/minidx12")
    target_link_libraries(rpcs3_emu PUBLIC d3d12 dxgi d3dcompiler)
endif()
# Remove desktop backends and all USB peripheral implementations. With no
# validated UWP USB transport, guest USB calls explicitly return ENOSYS.
get_target_property(embedded_sources rpcs3_emu SOURCES)
list(FILTER embedded_sources EXCLUDE REGEX "(^|/)(Audio/(Cubeb|XAudio2)/.*|Cell/lv2/sys_usbd|NP/upnp_handler|Io/(Buzz|Dimensions|GameTablet|GHLtar|GunCon3|Infinity|KamenRider|RB3MidiDrums|RB3MidiGuitar|RB3MidiKeyboard|Skylander|TopShotElite|TopShotFearmaster|Turntable|usb_device|usb_microphone|usb_vfs|usio|LogitechG27))\\.cpp$")
set_property(TARGET rpcs3_emu PROPERTY SOURCES "${embedded_sources}")
target_sources(rpcs3_emu PRIVATE
    Embedded/unsupported_usb.cpp
    Embedded/unsupported_upnp.cpp
    rpcs3_version.cpp
    Input/camera_video_sink.cpp
    Input/mouse_gyro_handler.cpp
    Input/pad_thread.cpp
    Input/product_info.cpp
    Input/ps_move_config.cpp
    Input/ps_move_tracker.cpp
    Input/raw_mouse_config.cpp
    Input/sdl_camera_handler.cpp
    Input/sdl_camera_video_sink.cpp
    Input/sdl_instance.cpp
    Input/sdl_pad_handler.cpp
)
target_link_libraries(rpcs3_emu PUBLIC 3rdparty::fusion)

add_library(rpcs3_core SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/Embedded/core_api.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Embedded/host_runtime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Embedded/brokered_files.cpp
)
target_compile_definitions(rpcs3_core PRIVATE RPCS3_CORE_EXPORTS UNICODE _UNICODE)
if(RPCS3_UWP_D3D12)
    target_compile_definitions(rpcs3_core PRIVATE RPCS3_UWP_D3D12=1)
    target_include_directories(rpcs3_core PRIVATE "${CMAKE_SOURCE_DIR}/3rdparty/minidx12")
endif()
target_compile_features(rpcs3_core PRIVATE cxx_std_23)
target_compile_options(rpcs3_core PRIVATE /EHsc)
target_include_directories(rpcs3_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Embedded)
target_link_libraries(rpcs3_core PRIVATE rpcs3_emu ws2_32 Iphlpapi Psapi windowsapp)
# Static module registration must survive extraction of the emulator archive.
target_link_options(rpcs3_core PRIVATE /WINMD:NO "/WHOLEARCHIVE:$<TARGET_FILE:rpcs3_emu>")
set_target_properties(rpcs3_core PROPERTIES OUTPUT_NAME rpcs3-core)
