cmake_minimum_required(VERSION 3.25)

# The version of the next release. CI passes -DMODELHUB_VERSION=<version> worked out from the git tags
# (scripts/next_version.ps1); this value is used only before the first tag and for local builds.
if(NOT DEFINED MODELHUB_VERSION)
  set(MODELHUB_VERSION "0.1.0")
endif()
project(ModelHub VERSION ${MODELHUB_VERSION} LANGUAGES CXX RC)

# ModelHub: everything local LLM applications on this PC share about models and llama.cpp.
#   ModelHubSdk.dll  model selection (catalog, hardware- and workload-based advice), the shared model store,
#                    verified resumable downloads, custom Hugging Face models, llama.cpp servers on demand;
#                    a plain C interface (include/modelhub/modelhub.h, C++14 wrapper modelhub.hpp)
#   ModelHub.exe     the same core as a localhost HTTP service (OpenAI-compatible /v1 included), so several
#                    applications can share running llama.cpp servers (client: modelhub_client.h)
#   llama.cpp        llama-server, downloaded and verified at configure time
# The SDK for applications is staged in build/sdk (see cmake/ModelHubConfig.cmake.in).

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(NOT WIN32)
  message(FATAL_ERROR "ModelHub currently targets Windows.")
endif()

if(NOT DEFINED MODELHUB_BUILD_NUMBER)
  set(MODELHUB_BUILD_NUMBER 0)  # CI passes its run number; it becomes the fourth part of the file version
endif()
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
                        MODELHUB_VERSION="${PROJECT_VERSION}"
                        MODELHUB_VER_MAJOR=${PROJECT_VERSION_MAJOR} MODELHUB_VER_MINOR=${PROJECT_VERSION_MINOR}
                        MODELHUB_VER_PATCH=${PROJECT_VERSION_PATCH} MODELHUB_VER_BUILD=${MODELHUB_BUILD_NUMBER})
add_compile_options(/W4 /permissive- /utf-8 /EHsc /MP)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)

include(cmake/Llama.cmake)  # sets MODELHUB_SDK_DIR, stages llama.cpp in ${MODELHUB_SDK_DIR}/bin/llama

# ---------------------------------------------------------------- core (one implementation for DLL and service)
add_library(ModelHubCore STATIC
  src/core/Text.cpp
  src/core/Hash.cpp
  src/core/HttpClient.cpp
  src/core/ProcessRunner.cpp
  src/core/Paths.cpp
  src/core/Config.cpp
  src/core/Catalog.cpp
  src/core/HuggingFace.cpp
  src/core/HfBrowse.cpp
  src/core/Hardware.cpp
  src/core/Advisor.cpp
  src/core/Store.cpp
  src/core/Downloader.cpp
  src/core/ServerPool.cpp
  src/core/HubCore.cpp
)
target_include_directories(ModelHubCore PUBLIC src/core)
target_link_libraries(ModelHubCore PUBLIC nlohmann_json::nlohmann_json PRIVATE winhttp bcrypt dxgi shell32)

# ---------------------------------------------------------------- ModelHubSdk.dll (C interface)
add_library(ModelHubSdk SHARED src/sdk/ModelHubSdk.cpp src/sdk/ModelHubSdk.rc)
target_include_directories(ModelHubSdk PUBLIC include)
target_compile_definitions(ModelHubSdk PRIVATE MODELHUB_SDK_EXPORTS)
target_link_libraries(ModelHubSdk PRIVATE ModelHubCore)

# ---------------------------------------------------------------- the hub service (+ command line)
add_library(ModelHubService STATIC src/hub/HubServer.cpp)
target_include_directories(ModelHubService PUBLIC src/hub include)
target_link_libraries(ModelHubService PUBLIC ModelHubCore httplib::httplib)

add_executable(ModelHub src/hub/main.cpp src/hub/ModelHub.rc)
target_link_libraries(ModelHub PRIVATE ModelHubService winhttp)
add_custom_command(TARGET ModelHub POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/config/catalog.json $<TARGET_FILE_DIR:ModelHub>/catalog.json)
target_include_directories(ModelHub PRIVATE src/manager)  # the icon (res/modelhub.ico)

# ---------------------------------------------------------------- ModelHubManager.exe (tray + model manager)
# Notification-area icon that starts, stops and restarts the service, and a window that suggests, downloads and
# deletes models for every application. Plain Win32 (no MFC), the same core as the DLL.
add_executable(ModelHubManager WIN32
  src/manager/main.cpp
  src/manager/ManagerWindow.cpp
  src/manager/HfBrowser.cpp
  src/manager/ModelHubManager.rc
)
target_include_directories(ModelHubManager PRIVATE src/manager include)
target_link_libraries(ModelHubManager PRIVATE ModelHubCore comctl32 shell32 ole32 winhttp)

# ---------------------------------------------------------------- SDK staging: build/sdk
# include/  headers;  lib/<Config>/ModelHubSdk.lib;  bin/<Config>/ModelHubSdk.dll, ModelHub.exe, ModelHubManager.exe;
# bin/catalog.json;  bin/llama/ (llama-server);  ModelHubConfig.cmake
foreach(header modelhub.h modelhub.hpp modelhub_client.h)
  configure_file(include/modelhub/${header} ${MODELHUB_SDK_DIR}/include/modelhub/${header} COPYONLY)
endforeach()
configure_file(config/catalog.json ${MODELHUB_SDK_DIR}/bin/catalog.json COPYONLY)
configure_file(cmake/ModelHubConfig.cmake.in ${MODELHUB_SDK_DIR}/ModelHubConfig.cmake @ONLY)
include(CMakePackageConfigHelpers)  # find_package(ModelHub 0.2 ...) accepts 0.2.x
write_basic_package_version_file(${MODELHUB_SDK_DIR}/ModelHubConfigVersion.cmake
                                 COMPATIBILITY SameMinorVersion ARCH_INDEPENDENT)
add_custom_command(TARGET ModelHubSdk POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${MODELHUB_SDK_DIR}/bin/$<CONFIG> ${MODELHUB_SDK_DIR}/lib/$<CONFIG>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ModelHubSdk> $<$<CONFIG:Debug>:$<TARGET_PDB_FILE:ModelHubSdk>> ${MODELHUB_SDK_DIR}/bin/$<CONFIG>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_LINKER_FILE:ModelHubSdk> ${MODELHUB_SDK_DIR}/lib/$<CONFIG>)
add_custom_command(TARGET ModelHub POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${MODELHUB_SDK_DIR}/bin/$<CONFIG>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ModelHub> ${MODELHUB_SDK_DIR}/bin/$<CONFIG>)
add_custom_command(TARGET ModelHubManager POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${MODELHUB_SDK_DIR}/bin/$<CONFIG>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ModelHubManager> ${MODELHUB_SDK_DIR}/bin/$<CONFIG>)

# ---------------------------------------------------------------- headers compiled as C++14 (AppAssist's level)
add_library(ModelHubCxx14Check STATIC tests/ClientCxx14.cpp)
target_include_directories(ModelHubCxx14Check PUBLIC include)
set_target_properties(ModelHubCxx14Check PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_link_libraries(ModelHubCxx14Check PUBLIC winhttp ModelHubSdk)

# ---------------------------------------------------------------- tests
enable_testing()
find_package(GTest CONFIG REQUIRED)
add_executable(ModelHubTests
  tests/CatalogTests.cpp
  tests/AdvisorTests.cpp
  tests/StoreTests.cpp
  tests/HubServerTests.cpp
  tests/SdkTests.cpp
  tests/LlamaTests.cpp
  tests/HfBrowseTests.cpp
)
target_link_libraries(ModelHubTests PRIVATE ModelHubService ModelHubCxx14Check ModelHubSdk GTest::gtest GTest::gtest_main)
target_compile_definitions(ModelHubTests PRIVATE MODELHUB_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
                                                 MODELHUB_BINARY_DIR="${CMAKE_BINARY_DIR}")
include(GoogleTest)
gtest_discover_tests(ModelHubTests DISCOVERY_MODE PRE_TEST)

# ---------------------------------------------------------------- install + package (the SDK layout)
install(TARGETS ModelHub ModelHubManager ModelHubSdk RUNTIME DESTINATION bin/$<CONFIG> ARCHIVE DESTINATION lib/$<CONFIG>)
install(FILES config/catalog.json DESTINATION bin)
install(FILES README.md LICENSE THIRD_PARTY_NOTICES.md DESTINATION .)
install(FILES ${MODELHUB_SDK_DIR}/ModelHubConfig.cmake ${MODELHUB_SDK_DIR}/ModelHubConfigVersion.cmake
        DESTINATION .)
install(DIRECTORY include/ DESTINATION include)
if(MODELHUB_BUNDLE_LLAMA)
  install(DIRECTORY ${MODELHUB_LLAMA_DIR}/ DESTINATION bin/llama PATTERN ".modelhub-*" EXCLUDE)
endif()
set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin/Release)
include(InstallRequiredSystemLibraries)
set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_FILE_NAME "ModelHub-${PROJECT_VERSION}-win64")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
