add_executable(status_test status_test.c)
target_link_libraries(status_test PRIVATE naula naula_allocator_counting)
naula_strict_warnings(status_test)
add_test(NAME status_test COMMAND status_test)
set_tests_properties(status_test PROPERTIES TIMEOUT 120)

function(naula_test name)
    add_executable(${name} ${name}.c ${ARGN})
    naula_strict_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

naula_test(sha256_test)
target_link_libraries(sha256_test PRIVATE naula naula_allocator)

set(NAULA_CONFORMANCE_DIR "${CMAKE_SOURCE_DIR}/siblings/conformance" CACHE PATH "The conformance kit checkout naula's tests read vectors from")
if(NOT EXISTS "${NAULA_CONFORMANCE_DIR}/vectors/standard/identity")
    message(FATAL_ERROR "the conformance kit is missing at ${NAULA_CONFORMANCE_DIR}: check it out at the commit repo.json pins, or set NAULA_CONFORMANCE_DIR")
endif()

naula_test(id_test)
target_link_libraries(id_test PRIVATE naula naula_allocator)
target_compile_definitions(id_test PRIVATE NAULA_IDENTITY_VECTORS="${NAULA_CONFORMANCE_DIR}/vectors/standard/identity")

naula_test(transport_sink_test)
target_link_libraries(transport_sink_test PRIVATE naula naula_allocator)
target_compile_definitions(transport_sink_test PRIVATE NAULA_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}")

naula_test(transport_shipped_test http_fixture.c)
target_link_libraries(transport_shipped_test PRIVATE naula naula_allocator)

naula_test(transport_network_test)
target_link_libraries(transport_network_test PRIVATE naula naula_allocator)
set_tests_properties(transport_network_test PROPERTIES LABELS network)

naula_test(transport_test http_fixture.c)
target_link_libraries(transport_test PRIVATE naula_test_hooks naula_allocator)

if(WIN32)
    target_link_libraries(transport_shipped_test PRIVATE ws2_32)
    target_link_libraries(transport_test PRIVATE ws2_32)
endif()
