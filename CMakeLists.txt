# It's recommended to set a minimum CMake version.
# If you use CMake features from higher versions, update this to match.
cmake_minimum_required(VERSION 3.21)

# Set your project name. This will be the name of your SKSE .dll file.
project(PluginDisabler VERSION 0.0.1 LANGUAGES CXX)

# If you're not using a mod manager, you probably want the SKSE plugin to go
# inside of your Skyrim "Data" folder.
#
# To do this automatically, set the `SKYRIM_FOLDER` environment variable
# to the path of your Skyrim Special Edition folder
if(DEFINED ENV{SKYRIM_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_FOLDER}/Data")
	set(OUTPUT_FOLDER "$ENV{SKYRIM_FOLDER}/Data")
endif()

# If you're using Mod Organizer 2 or Vortex, you might want this to go inside
# of your "mods" folder, inside of a subfolder named "<your mod>".
#
# To do this automatically, set the `SKYRIM_MODS_FOLDER` environment variable
# to the path of your "mods" folder
if(DEFINED ENV{SKYRIM_MODS_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_MODS_FOLDER}")
	set(OUTPUT_FOLDER "$ENV{SKYRIM_MODS_FOLDER}/${PROJECT_NAME}")
endif()

# Otherwise, you can set OUTPUT_FOLDER to any place you'd like :)
# set(OUTPUT_FOLDER "C:/path/to/any/folder")

# --- CommonLibSSE-NG as a git submodule --------------------------------------
#
# One-time setup from your project root:
#   git submodule add https://github.com/CharmedBaryon/CommonLibSSE-NG lib/CommonLibSSE-NG
#   git submodule update --init --recursive
#
# Options must be set BEFORE add_subdirectory, since the library reads them
# while it is being processed.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)

# Build CommonLibSSE-NG from source alongside your plugin.
# (Replaces: find_package(CommonLibSSE CONFIG REQUIRED))
add_subdirectory(lib/CommonLibSSE-NG)

# Setup your SKSE plugin as an SKSE plugin!
#
# add_commonlibsse_plugin() isn't available when CommonLibSSE-NG is built from
# source as a submodule, so we set up the DLL target by hand instead.
set(PLUGIN_AUTHOR "AdoringStan")
configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/PluginInfo.cpp.in
	${CMAKE_CURRENT_BINARY_DIR}/PluginInfo.cpp
	@ONLY
)
add_library(${PROJECT_NAME} SHARED
	src/main.cpp
	"${CMAKE_CURRENT_BINARY_DIR}/PluginInfo.cpp"
) # <--- specifies main.cpp
target_link_libraries(${PROJECT_NAME} PRIVATE CommonLibSSE::CommonLibSSE)
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json)
target_compile_options(${PROJECT_NAME}
	PRIVATE
		-Wno-invalid-noreturn
		-Wno-tautological-constant-out-of-range-compare
)
target_compile_features(${PROJECT_NAME}
	PRIVATE cxx_std_23
) # <--- use C++23 standard
target_precompile_headers(${PROJECT_NAME}
	PRIVATE src/PCH.h
) # <--- PCH.h is required!

# When your SKSE .dll is compiled, this will automatically copy the .dll into your mods folder.
# Only works if OUTPUT_FOLDER is set above (or via the environment variables).
if(DEFINED OUTPUT_FOLDER)
	# Copy the SKSE plugin .dll files into the SKSE/Plugins/ folder
	set(DLL_FOLDER "${OUTPUT_FOLDER}/SKSE/Plugins")

	message(STATUS "SKSE plugin output folder: ${DLL_FOLDER}")

	add_custom_command(
		TARGET "${PROJECT_NAME}"
		POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different
			"$<TARGET_FILE:${PROJECT_NAME}>"
			"${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
		VERBATIM
	)

	# If you perform a "Debug" build, also copy .pdb file (for debug symbols)
	if(CMAKE_BUILD_TYPE STREQUAL "Debug")
		add_custom_command(
			TARGET "${PROJECT_NAME}"
			POST_BUILD
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"$<TARGET_PDB_FILE:${PROJECT_NAME}>"
				"${DLL_FOLDER}/$<TARGET_PDB_FILE_NAME:${PROJECT_NAME}>"
			VERBATIM
		)
	endif()
endif()
