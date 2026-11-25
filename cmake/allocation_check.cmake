set(NAULA_PLAIN_ALLOCATION
    "(^|[^A-Za-z0-9_.>])\\(?[ \t]*(malloc|calloc|realloc|free|strdup|_strdup)[ \t]*\\)?[ \t]*\\(")

function(naula_plain_allocations_in directory out_variable)
    file(GLOB_RECURSE sources "${directory}/*.c" "${directory}/*.h")
    set(found "")
    foreach(source IN LISTS sources)
        if(source MATCHES "/src/memory_malloc\\.c$")
            continue()
        endif()
        file(STRINGS "${source}" calls REGEX "${NAULA_PLAIN_ALLOCATION}")
        foreach(call IN LISTS calls)
            string(STRIP "${call}" call)
            string(APPEND found "\n  ${source}: ${call}")
        endforeach()
    endforeach()
    set(${out_variable} "${found}" PARENT_SCOPE)
endfunction()

#[[
Refuses a shipped source that allocates or releases past the binary's one allocator, since a test's counting
allocator cannot fail a plain malloc. src/memory_malloc.c is the allocator itself.
]]
function(naula_refuse_plain_allocation)
    set(found "")
    foreach(directory IN LISTS ARGN)
        naula_plain_allocations_in("${directory}" in_directory)
        string(APPEND found "${in_directory}")
    endforeach()
    if(NOT found STREQUAL "")
        message(FATAL_ERROR "allocate through naula_memory_allocate and release through naula_memory_release:${found}")
    endif()
endfunction()
