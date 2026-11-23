cmake_minimum_required(VERSION 3.16)
project(hydration VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
foreach(CONFIG_TYPE Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${CONFIG_TYPE}" CONFIG_UPPER)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} "${CMAKE_BINARY_DIR}")
endforeach()

function(configure_standalone_target TARGET_NAME)
    target_include_directories(${TARGET_NAME} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )

    if(WIN32)
        target_compile_definitions(${TARGET_NAME} PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            _CRT_SECURE_NO_WARNINGS
        )
    endif()

    if(MINGW)
        target_compile_options(${TARGET_NAME} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            $<$<CONFIG:Release>:-O3>
            $<$<CONFIG:Release>:-DNDEBUG>
            $<$<CONFIG:Release>:-fno-exceptions>
            $<$<CONFIG:Release>:-fno-rtti>
            $<$<CONFIG:Release>:-ffunction-sections>
            $<$<CONFIG:Release>:-fdata-sections>
        )
        # Links libstdc++, libgcc, and winpthread statically; Windows UCRT stays OS-native dynamic
        target_link_options(${TARGET_NAME} PRIVATE
            -static
            $<$<CONFIG:Release>:-Wl,--gc-sections>
            $<$<CONFIG:Release>:-Wl,-s>
        )
    elseif(MSVC)
        target_compile_options(${TARGET_NAME} PRIVATE
            /W4
            /utf-8
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/DNDEBUG>
            $<$<CONFIG:Release>:/EHs-c->
            $<$<CONFIG:Release>:/GR->
            $<$<CONFIG:Release>:/Gw>
            $<$<CONFIG:Release>:/Gy>
            $<$<CONFIG:Release>:/MT>
        )
        target_compile_definitions(${TARGET_NAME} PRIVATE
            $<$<CONFIG:Release>:_HAS_EXCEPTIONS=0>
        )
        target_link_options(${TARGET_NAME} PRIVATE
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>
        )
    else()
        target_compile_options(${TARGET_NAME} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            $<$<CONFIG:Release>:-O3>
            $<$<CONFIG:Release>:-DNDEBUG>
            $<$<CONFIG:Release>:-fno-exceptions>
            $<$<CONFIG:Release>:-fno-rtti>
            $<$<CONFIG:Release>:-ffunction-sections>
            $<$<CONFIG:Release>:-fdata-sections>
        )
        target_link_options(${TARGET_NAME} PRIVATE
            $<$<CONFIG:Release>:-Wl,--gc-sections>
            $<$<CONFIG:Release>:-Wl,-s>
        )
    endif()
endfunction()

# Primary CLI executable
add_executable(hydration
    src/main.cpp
)
configure_standalone_target(hydration)

# Optional test validator binary
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/test_validator.cpp")
    add_executable(test_validator
        src/test_validator.cpp
    )
    configure_standalone_target(test_validator)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_validator.cpp")
    add_executable(test_validator
        test_validator.cpp
    )
    configure_standalone_target(test_validator)
endif()

# Copy built Release binaries to project root
add_custom_command(TARGET hydration POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:hydration>"
        "${CMAKE_CURRENT_SOURCE_DIR}/hydration.exe"
    COMMENT "Copying hydration.exe to project root"
)

if(TARGET test_validator)
    add_custom_command(TARGET test_validator POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:test_validator>"
            "${CMAKE_CURRENT_SOURCE_DIR}/test_validator.exe"
        COMMENT "Copying test_validator.exe to project root"
    )
endif()