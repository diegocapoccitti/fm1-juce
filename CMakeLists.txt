cmake_minimum_required(VERSION 3.22)

# iOS: configure with -G Xcode -DCMAKE_SYSTEM_NAME=iOS (-DFM1_IOS_SIMULATOR=ON for the Simulator)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum iOS")
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
else()
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS")
endif()

project(FM1 VERSION 0.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The Rust core is built for this machine's architecture (Homebrew's Rust has only the host
# target). For a universal binary install rustup, `rustup target add x86_64-apple-darwin
# aarch64-apple-darwin` and configure with -DFM1_UNIVERSAL=ON.
option(FM1_UNIVERSAL "Build the Rust core for arm64 and x86_64 and lipo them" OFF)
set(FM1_IOS OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(FM1_IOS ON)
endif()
option(FM1_IOS_SIMULATOR "iOS: build the Rust core for the Simulator" OFF)
set(FM1_TEAM_ID "" CACHE STRING "iOS: the Apple development team that signs the app")

if(APPLE AND NOT FM1_IOS)
    if(FM1_UNIVERSAL)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    elseif(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}" CACHE STRING "" FORCE)
    endif()
endif()

set(JUCE_DIR "/Applications/JUCE" CACHE PATH "JUCE checkout")
set(AAX_SDK_DIR "${JUCE_DIR}/aax-sdk-2-8-0" CACHE PATH "AAX SDK")
add_subdirectory(${JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)

set(FM1_FORMATS AU VST3 Standalone)
if(FM1_IOS)
    set(FM1_FORMATS AUv3 Standalone)
elseif(EXISTS "${AAX_SDK_DIR}/Interfaces")
    juce_set_aax_sdk_path(${AAX_SDK_DIR})
    list(APPEND FM1_FORMATS AAX)
endif()

# ----------------------------------------------------------------- the Rust core --
if(FM1_IOS)
    # rustup's toolchain: it has the iOS standard libraries (`rustup target add aarch64-apple-ios aarch64-apple-ios-sim`)
    find_program(CARGO cargo HINTS $ENV{HOME}/.rustup/toolchains/stable-aarch64-apple-darwin/bin NO_DEFAULT_PATH REQUIRED)
else()
    find_program(CARGO cargo HINTS $ENV{HOME}/.cargo/bin /opt/homebrew/bin /usr/local/bin REQUIRED)
endif()
get_filename_component(CARGO_BIN ${CARGO} DIRECTORY)
set(RUST_DIR ${CMAKE_SOURCE_DIR}/rust/fm1-ffi)
set(RUST_TARGET_DIR ${CMAKE_BINARY_DIR}/rust-target)
if(WIN32)
    set(RUST_LIB ${RUST_TARGET_DIR}/release/fm1_ffi.lib)
else()
    set(RUST_LIB ${RUST_TARGET_DIR}/fm1/libfm1_ffi.a)
endif()

file(GLOB_RECURSE RUST_SOURCES CONFIGURE_DEPENDS
    ${RUST_DIR}/src/*.rs
    ${CMAKE_SOURCE_DIR}/external/fm1-emu/crates/fm1-core/src/*.rs
    ${CMAKE_SOURCE_DIR}/external/fm1-emu/crates/fm1-soc/src/*.rs
    ${CMAKE_SOURCE_DIR}/external/fm1-emu/crates/fm1-isa/src/*.rs
    ${CMAKE_SOURCE_DIR}/external/fm1-emu/crates/fm1-plugin/src/engine.rs
    ${CMAKE_SOURCE_DIR}/external/fm1-emu/crates/fm1-plugin/src/savefile.rs)

if(FM1_IOS)
    # (no JIT on iOS: fm1-core interprets there)
    if(FM1_IOS_SIMULATOR)
        set(RUST_TRIPLE aarch64-apple-ios-sim)
    else()
        set(RUST_TRIPLE aarch64-apple-ios)
    endif()
    add_custom_command(OUTPUT ${RUST_LIB}
        COMMAND ${CMAKE_COMMAND} -E env IPHONEOS_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET} RUSTC=${CARGO_BIN}/rustc
                ${CARGO} build --release --manifest-path ${RUST_DIR}/Cargo.toml --target ${RUST_TRIPLE} --target-dir ${RUST_TARGET_DIR}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${RUST_TARGET_DIR}/fm1
        COMMAND ${CMAKE_COMMAND} -E copy ${RUST_TARGET_DIR}/${RUST_TRIPLE}/release/libfm1_ffi.a ${RUST_LIB}
        DEPENDS ${RUST_SOURCES} ${RUST_DIR}/Cargo.toml
        COMMENT "Building the FM-1 emulator core (Rust, ${RUST_TRIPLE})" VERBATIM)
elseif(APPLE AND FM1_UNIVERSAL)
    add_custom_command(OUTPUT ${RUST_LIB}
        COMMAND ${CARGO} build --release --manifest-path ${RUST_DIR}/Cargo.toml --target aarch64-apple-darwin --target-dir ${RUST_TARGET_DIR}
        COMMAND ${CARGO} build --release --manifest-path ${RUST_DIR}/Cargo.toml --target x86_64-apple-darwin --target-dir ${RUST_TARGET_DIR}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${RUST_TARGET_DIR}/fm1
        COMMAND lipo -create ${RUST_TARGET_DIR}/aarch64-apple-darwin/release/libfm1_ffi.a ${RUST_TARGET_DIR}/x86_64-apple-darwin/release/libfm1_ffi.a -output ${RUST_LIB}
        DEPENDS ${RUST_SOURCES} ${RUST_DIR}/Cargo.toml
        COMMENT "Building the FM-1 emulator core (Rust, universal)" VERBATIM)
elseif(APPLE)
    add_custom_command(OUTPUT ${RUST_LIB}
        COMMAND ${CMAKE_COMMAND} -E env MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
                ${CARGO} build --release --manifest-path ${RUST_DIR}/Cargo.toml --target-dir ${RUST_TARGET_DIR}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${RUST_TARGET_DIR}/fm1
        COMMAND ${CMAKE_COMMAND} -E copy ${RUST_TARGET_DIR}/release/libfm1_ffi.a ${RUST_LIB}
        DEPENDS ${RUST_SOURCES} ${RUST_DIR}/Cargo.toml
        COMMENT "Building the FM-1 emulator core (Rust)" VERBATIM)
else()
    add_custom_command(OUTPUT ${RUST_LIB}
        COMMAND ${CARGO} build --release --manifest-path ${RUST_DIR}/Cargo.toml --target-dir ${RUST_TARGET_DIR}
        DEPENDS ${RUST_SOURCES} ${RUST_DIR}/Cargo.toml
        COMMENT "Building the FM-1 emulator core (Rust)" VERBATIM)
endif()
add_custom_target(fm1_rust DEPENDS ${RUST_LIB})

add_library(fm1_core STATIC IMPORTED GLOBAL)
set_target_properties(fm1_core PROPERTIES IMPORTED_LOCATION ${RUST_LIB})
add_dependencies(fm1_core fm1_rust)
if(WIN32)
    set_target_properties(fm1_core PROPERTIES INTERFACE_LINK_LIBRARIES "ws2_32;userenv;ntdll;bcrypt;advapi32")
endif()

# ------------------------------------------------------------------- the plugin --
if(EXISTS ${CMAKE_SOURCE_DIR}/Resources/AppIcon/icon.png)
    set(FM1_ICON ICON_BIG ${CMAKE_SOURCE_DIR}/Resources/AppIcon/icon.png)
endif()

juce_add_plugin(FM1
    ${FM1_ICON}
    COMPANY_NAME "34Audiovisuals"
    COMPANY_WEBSITE "https://github.com/joakimeriksson/fm1-emu"
    BUNDLE_ID "com.34audiovisuals.fm1"
    PRODUCT_NAME "FM-1"
    DESCRIPTION "The M-VAVE FM-1 emulated: its stock and community firmwares as an instrument"
    PLUGIN_MANUFACTURER_CODE Av34
    PLUGIN_CODE Fm1j
    FORMATS ${FM1_FORMATS}
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    # Pro Tools reads /Library (needs admin): the AAX is left in the build, see README
    AAX_COPY_DIR "${CMAKE_BINARY_DIR}/aax-out"
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    VST3_CATEGORIES Instrument Synth
    AAX_CATEGORY AAX_ePlugInCategory_SWGenerators
    MICROPHONE_PERMISSION_ENABLED FALSE
    # iOS
    IPHONE_SCREEN_ORIENTATIONS UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight
    IPAD_SCREEN_ORIENTATIONS UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight
    STATUS_BAR_HIDDEN TRUE
    REQUIRES_FULL_SCREEN TRUE
    BACKGROUND_AUDIO_ENABLED TRUE
    BACKGROUND_BLE_ENABLED TRUE
    FILE_SHARING_ENABLED TRUE
    DOCUMENT_BROWSER_ENABLED FALSE)

if(FM1_IOS AND FM1_TEAM_ID)
    set_target_properties(FM1 FM1_Standalone FM1_AUv3 PROPERTIES
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ${FM1_TEAM_ID}
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development")
endif()

file(GLOB FM1_RESOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/Resources/*.png ${CMAKE_SOURCE_DIR}/Resources/*.jpg)
if(FM1_RESOURCES)
    juce_add_binary_data(FM1Assets SOURCES ${FM1_RESOURCES})
    target_compile_definitions(FM1 PRIVATE FM1_HAS_ASSETS=1)
    target_link_libraries(FM1 PRIVATE FM1Assets)
endif()

# iOS: a firmware comes with the app (Felucca), loaded the first time
if(FM1_IOS AND EXISTS ${CMAKE_SOURCE_DIR}/Resources/Firmware/felucca-1.7.0.fwsc)
    juce_add_binary_data(FM1Firmware HEADER_NAME FirmwareData.h NAMESPACE FirmwareData
        SOURCES ${CMAKE_SOURCE_DIR}/Resources/Firmware/felucca-1.7.0.fwsc)
    target_compile_definitions(FM1 PRIVATE FM1_BUNDLED_FIRMWARE=1)
    target_link_libraries(FM1 PRIVATE FM1Firmware)
endif()

target_sources(FM1 PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/PanelRenderer.cpp)

target_compile_definitions(FM1 PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(FM1
    PRIVATE
        fm1_core
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
