cmake_minimum_required(VERSION 3.19)
project(naula C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(naula_is_top_level ON)
else()
    set(naula_is_top_level OFF)
endif()
option(NAULA_TESTS "Build naula's tests" ${naula_is_top_level})

function(naula_strict_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8 /D_CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

add_library(naula STATIC src/status.c)
target_include_directories(naula PUBLIC include)
set_target_properties(naula PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
naula_strict_warnings(naula)

# naula carries no allocator: every binary links exactly one of these two, so a test can fail an allocation.
add_library(naula_allocator OBJECT src/memory_malloc.c)
target_include_directories(naula_allocator PUBLIC include)
set_target_properties(naula_allocator PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
naula_strict_warnings(naula_allocator)

add_library(naula_allocator_counting OBJECT EXCLUDE_FROM_ALL src/memory_malloc.c)
target_include_directories(naula_allocator_counting PUBLIC include src)
target_compile_definitions(naula_allocator_counting PUBLIC NAULA_TESTING)
set_target_properties(naula_allocator_counting PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
naula_strict_warnings(naula_allocator_counting)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/allocation_check.cmake)
naula_refuse_plain_allocation(${CMAKE_CURRENT_SOURCE_DIR}/src)

if(NAULA_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
