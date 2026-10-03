if(NOT DEFINED UWP_IMGUI_FRONTEND_UWP_BUILD)
	set(UWP_IMGUI_FRONTEND_UWP_BUILD OFF)
	if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
		set(UWP_IMGUI_FRONTEND_UWP_BUILD ON)
	endif()
endif()

add_library(RPCS3UwpFrontendHost STATIC
	MinimalHost.cpp
	MinimalHost.h
	SampleInputMappingHost.cpp
	SampleInputMappingHost.h
)

target_compile_features(RPCS3UwpFrontendHost PRIVATE cxx_std_20)
target_include_directories(RPCS3UwpFrontendHost PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_definitions(RPCS3UwpFrontendHost PRIVATE
	_WIN32_WINNT=0x0A00
	_CRT_SECURE_NO_WARNINGS
	NOMINMAX
	WIN32_LEAN_AND_MEAN
)
target_link_libraries(RPCS3UwpFrontendHost PUBLIC
	UwpImGuiFrontend
	windowsapp
)

if(MSVC)
	set_property(TARGET RPCS3UwpFrontendHost PROPERTY
		MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>$<$<BOOL:${UWP_IMGUI_FRONTEND_UWP_BUILD}>:DLL>")
	target_compile_options(RPCS3UwpFrontendHost PRIVATE /EHsc)
endif()

set_target_properties(RPCS3UwpFrontendHost PROPERTIES FOLDER "RPCS3-UWP")

if(WIN32 AND CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
	add_library(RPCS3UwpImGui STATIC
		"${UWP_IMGUI_FRONTEND_IMGUI_DIR}/imgui.cpp"
		"${UWP_IMGUI_FRONTEND_IMGUI_DIR}/imgui_draw.cpp"
		"${UWP_IMGUI_FRONTEND_IMGUI_DIR}/imgui_tables.cpp"
		"${UWP_IMGUI_FRONTEND_IMGUI_DIR}/imgui_widgets.cpp"
		"${UWP_IMGUI_FRONTEND_IMGUI_DIR}/backends/imgui_impl_dx12.cpp"
	)
	target_include_directories(RPCS3UwpImGui PUBLIC
		"${UWP_IMGUI_FRONTEND_IMGUI_DIR}"
		"${UWP_IMGUI_FRONTEND_IMGUI_DIR}/backends"
	)
	target_compile_definitions(RPCS3UwpImGui PRIVATE
		UWP_IMGUI_FRONTEND_UWP
		_WIN32_WINNT=0x0A00
		_CRT_SECURE_NO_WARNINGS
		NOMINMAX
		WIN32_LEAN_AND_MEAN
		_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
	)
	target_compile_options(RPCS3UwpImGui PRIVATE /EHsc)
	set_property(TARGET RPCS3UwpImGui PROPERTY
		MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
	target_link_libraries(RPCS3UwpImGui PRIVATE
		d3d12
		d3dcompiler
		dxgi
	)

	set(sample_manifest "${CMAKE_CURRENT_SOURCE_DIR}/UWP-App/Package.appxmanifest")
	set(sample_xaml
		"${CMAKE_CURRENT_SOURCE_DIR}/UWP-App/XamlApp.xaml"
		"${CMAKE_CURRENT_SOURCE_DIR}/UWP-App/XamlPage.xaml"
	)
	file(GLOB_RECURSE windows_contract_winmd
		"C:/Program Files (x86)/Windows Kits/10/References/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/*.winmd")
	set_source_files_properties(${windows_contract_winmd} PROPERTIES
		VS_TOOL_OVERRIDE Reference)
	set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/UWP-App/XamlApp.xaml"
		PROPERTIES VS_TOOL_OVERRIDE ApplicationDefinition)
	set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/UWP-App/XamlPage.xaml"
		PROPERTIES VS_TOOL_OVERRIDE Page)
	file(GLOB sample_assets CONFIGURE_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/UWP-App/Assets/*.png")
	list(FILTER sample_assets EXCLUDE REGEX
		"[/\\\\](StoreLogo|Square44x44Logo|Square150x150Logo)\\.png$")
	list(FILTER sample_assets EXCLUDE REGEX
		"[/\\\\]Square44x44Logo\\.targetsize-24_altform-unplated\\.png$")
	file(GLOB_RECURSE sample_frontend_assets CONFIGURE_DEPENDS
		"${UWP_IMGUI_FRONTEND_ASSET_DIR}/*")
	set_source_files_properties("${sample_manifest}" PROPERTIES
		VS_DEPLOYMENT_CONTENT 1)
	foreach(asset IN LISTS sample_assets)
		get_filename_component(asset_name "${asset}" NAME)
		set_source_files_properties("${asset}" PROPERTIES
			VS_DEPLOYMENT_CONTENT 1
			VS_DEPLOYMENT_LOCATION "Assets"
			VS_SETTINGS "DeploymentContent=true;DeploymentLocation=Assets;Link=Assets\\${asset_name}"
		)
	endforeach()
	foreach(asset IN LISTS sample_frontend_assets)
		file(RELATIVE_PATH asset_relative "${UWP_IMGUI_FRONTEND_ASSET_DIR}" "${asset}")
		get_filename_component(asset_location "${asset_relative}" DIRECTORY)
		set(deployment_location "resources")
		if(NOT asset_location STREQUAL "")
			set(deployment_location "resources/${asset_location}")
		endif()
		file(TO_NATIVE_PATH "resources/${asset_relative}" asset_link)
		set_source_files_properties("${asset}" PROPERTIES
			VS_DEPLOYMENT_CONTENT 1
			VS_DEPLOYMENT_LOCATION "${deployment_location}"
			VS_SETTINGS "DeploymentContent=true;DeploymentLocation=${deployment_location};Link=${asset_link}"
		)
	endforeach()

	add_library(RPCS3UwpRuntime SHARED
		UWP-App/App.cpp
		UWP-App/FrontendRuntime.h
		UWP-App/D3D12Renderer.cpp
		UWP-App/D3D12Renderer.h
	)
	target_compile_features(RPCS3UwpRuntime PRIVATE cxx_std_20)
	target_compile_definitions(RPCS3UwpRuntime PRIVATE
		UWP_IMGUI_FRONTEND_UWP RPCS3_XAML_HOST _WIN32_WINNT=0x0A00
		_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
	target_compile_options(RPCS3UwpRuntime PRIVATE /EHsc)
	target_include_directories(RPCS3UwpRuntime PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/UWP-App"
		"${UWP_IMGUI_FRONTEND_IMGUI_DIR}/backends")
	target_link_libraries(RPCS3UwpRuntime PUBLIC
		RPCS3UwpFrontendHost RPCS3UwpImGui
		d3d12 d3dcompiler dxgi windowsapp windowscodecs)
	set_property(TARGET RPCS3UwpRuntime PROPERTY
		MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
	set_target_properties(RPCS3UwpRuntime PROPERTIES
		OUTPUT_NAME "RPCS3FrontendRuntime"
		WINDOWS_EXPORT_ALL_SYMBOLS OFF)

	add_executable(RPCS3UwpApp WIN32
		UWP-App/XamlApp.xaml.cpp
		UWP-App/XamlApp.xaml.h
		UWP-App/XamlPage.xaml.cpp
		UWP-App/XamlPage.xaml.h
		${sample_xaml}
		${windows_contract_winmd}
		"${sample_manifest}"
		${sample_assets}
		${sample_frontend_assets}
	)
	# XAML-generated C++/CX requires /ZW and therefore C++17.
	target_compile_features(RPCS3UwpApp PRIVATE cxx_std_17)
	set_property(TARGET RPCS3UwpApp PROPERTY CXX_STANDARD 17)
	target_compile_definitions(RPCS3UwpApp PRIVATE
		UWP_IMGUI_FRONTEND_UWP
		RPCS3_XAML_HOST
		_WIN32_WINNT=0x0A00
		_CRT_SECURE_NO_WARNINGS
		NOMINMAX
		WIN32_LEAN_AND_MEAN
		_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
	)
	target_compile_options(RPCS3UwpApp PRIVATE /EHsc /ZW)
	target_include_directories(RPCS3UwpApp PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}"
		"${CMAKE_CURRENT_SOURCE_DIR}/UWP-App"
		"${UWP_IMGUI_FRONTEND_IMGUI_DIR}/backends"
	)
	target_link_options(RPCS3UwpApp PRIVATE
		/APPCONTAINER
	)
	target_link_libraries(RPCS3UwpApp PRIVATE
		windowsapp
	)
	add_dependencies(RPCS3UwpApp RPCS3UwpRuntime)
	add_custom_command(TARGET RPCS3UwpApp POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"$<TARGET_FILE:RPCS3UwpRuntime>"
			"$<TARGET_FILE_DIR:RPCS3UwpApp>/$<TARGET_FILE_NAME:RPCS3UwpRuntime>")
	set_property(TARGET RPCS3UwpApp PROPERTY
		MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
	set_target_properties(RPCS3UwpApp PROPERTIES
		OUTPUT_NAME "RPCS3-UWP"
		VS_WINRT_COMPONENT ON
		VS_WINRT_EXTENSIONS ON
		VS_GLOBAL_Keyword "DirectXXamlApp"
		VS_GLOBAL_CharacterSet "Unicode"
		VS_GLOBAL_DefaultLanguage "en-US"
		VS_GLOBAL_AppContainerApplication "true"
		VS_GLOBAL_ApplicationType "Windows Store"
		VS_GLOBAL_ApplicationTypeRevision "10.0"
		VS_GLOBAL_WindowsTargetPlatformMinVersion "10.0.19041.0"
		FOLDER "RPCS3-UWP"
	)
	uwp_imgui_frontend_copy_assets(RPCS3UwpApp
		"$<TARGET_FILE_DIR:RPCS3UwpApp>/resources")
endif()
