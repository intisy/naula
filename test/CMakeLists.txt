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
if(NOT WIN32)
    add_library(fake_curl_reporting_receive_error MODULE fake_curl_reporting_receive_error.c)
    add_dependencies(transport_test fake_curl_reporting_receive_error)
    target_compile_definitions(transport_test PRIVATE NAULA_FAKE_CURL="$<TARGET_FILE:fake_curl_reporting_receive_error>")
endif()

if(WIN32)
    target_link_libraries(transport_shipped_test PRIVATE ws2_32)
    target_link_libraries(transport_test PRIVATE ws2_32)
endif()

set(NAULA_FIXTURE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/git_fixtures")
naula_test(git_test)
target_link_libraries(git_test PRIVATE naula_git naula naula_allocator)
target_compile_definitions(git_test PRIVATE NAULA_FIXTURE_ROOT="${NAULA_FIXTURE_ROOT}")
add_test(NAME git_fixtures_build COMMAND ${CMAKE_COMMAND} -DROOT=${NAULA_FIXTURE_ROOT} -P ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/make_repositories.cmake)
add_test(NAME git_fixtures_remove COMMAND ${CMAKE_COMMAND} -E remove_directory ${NAULA_FIXTURE_ROOT})
set_tests_properties(git_fixtures_build PROPERTIES FIXTURES_SETUP git_fixtures TIMEOUT 120)
set_tests_properties(git_fixtures_remove PROPERTIES FIXTURES_CLEANUP git_fixtures)
set_tests_properties(git_test PROPERTIES FIXTURES_REQUIRED git_fixtures
    ENVIRONMENT "HOME=${NAULA_FIXTURE_ROOT}/home;USERPROFILE=${NAULA_FIXTURE_ROOT}/home;XDG_CONFIG_HOME=${NAULA_FIXTURE_ROOT}/home/xdg;GIT_CONFIG_GLOBAL=${NAULA_FIXTURE_ROOT}/home/gitconfig;GIT_CONFIG_NOSYSTEM=1")

naula_test(home_test)
target_link_libraries(home_test PRIVATE naula naula_allocator)

naula_test(provider_github_test http_fixture.c)
target_link_libraries(provider_github_test PRIVATE naula_test_hooks naula_allocator)
target_compile_definitions(provider_github_test PRIVATE NAULA_GITHUB_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/github")
if(WIN32)
    target_link_libraries(provider_github_test PRIVATE ws2_32)
endif()

naula_test(provider_gitea_test http_fixture.c)
target_link_libraries(provider_gitea_test PRIVATE naula_test_hooks naula_allocator)
target_compile_definitions(provider_gitea_test PRIVATE NAULA_FORGEJO_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/forgejo")

naula_test(provider_web_test http_fixture.c)
target_link_libraries(provider_web_test PRIVATE naula_test_hooks naula_allocator)

naula_test(provider_network_test)
target_link_libraries(provider_network_test PRIVATE naula naula_allocator)
set_tests_properties(provider_network_test PROPERTIES LABELS network)
if(WIN32)
    target_link_libraries(provider_gitea_test PRIVATE ws2_32)
    target_link_libraries(provider_web_test PRIVATE ws2_32)
endif()

naula_test(repo_file_test)
target_link_libraries(repo_file_test PRIVATE naula naula_allocator_counting)
target_compile_definitions(repo_file_test PRIVATE NAULA_REPO_FILE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/repo-file"
    NAULA_REPO_FILE_FORMAT_ONE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/repo-file-format-one/repo.json")
