# The test build of the naula program: the same sources with a forge a test describes, and file urls allowed.
add_executable(naula_cli_test EXCLUDE_FROM_ALL ${NAULA_CLI_SOURCES} ${PROJECT_SOURCE_DIR}/src/cli/test_forge.c)
target_include_directories(naula_cli_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(naula_cli_test PRIVATE NAULA_TEST_HOOKS)
target_link_libraries(naula_cli_test PRIVATE naula_git naula naula_allocator)
set_target_properties(naula_cli_test PROPERTIES OUTPUT_NAME naula RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/program)
naula_strict_warnings(naula_cli_test)

add_executable(cli_test cli_test.c)
add_dependencies(cli_test naula_cli_test)
target_include_directories(cli_test PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/test)
target_link_libraries(cli_test PRIVATE naula_git naula naula_allocator)
target_compile_definitions(cli_test PRIVATE NAULA_PROGRAM="$<TARGET_FILE:naula_cli_test>"
    NAULA_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}" NAULA_CMAKE="${CMAKE_COMMAND}")
naula_strict_warnings(cli_test)
# The driver builds paths and JSON in fixed buffers sized for its own fixtures; GCC cannot see those bounds.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(cli_test PRIVATE -Wno-format-truncation -Wno-restrict)
endif()

set(NAULA_CLI_HOME "${CMAKE_CURRENT_BINARY_DIR}/home")
file(MAKE_DIRECTORY "${NAULA_CLI_HOME}")
# Git for Windows stops at MAX_PATH inside nested checkouts unless told otherwise; other systems ignore the key.
file(WRITE "${NAULA_CLI_HOME}/gitconfig" "[core]\n\tlongpaths = true\n[init]\n\tdefaultBranch = main\n")
set(NAULA_CLI_ENVIRONMENT "HOME=${NAULA_CLI_HOME};USERPROFILE=${NAULA_CLI_HOME};XDG_CONFIG_HOME=${NAULA_CLI_HOME}/xdg;GIT_CONFIG_GLOBAL=${NAULA_CLI_HOME}/gitconfig;GIT_CONFIG_NOSYSTEM=1")

foreach(name IN ITEMS check_refuses_a_rebased_root check_warns_on_a_narrow_at_and_prints_the_line
        check_warns_on_a_url_dependency check_refuses_an_owner_repo_literal_outside_exempt_places
        siblings_clones_nested_paths siblings_refuses_a_copy sync_is_a_no_op_on_a_laid_out_root
        hold_adds_an_org_and_lays_it_out release_removes_an_orgs_links_but_never_a_dirty_clone
        clone_names_the_folder_by_twelve_hex)
    add_test(NAME cli/${name} COMMAND cli_test ${name})
    set_tests_properties(cli/${name} PROPERTIES TIMEOUT 300 ENVIRONMENT "${NAULA_CLI_ENVIRONMENT}")
endforeach()

add_executable(fake_old_git fake_old_git.c)
set_target_properties(fake_old_git PROPERTIES OUTPUT_NAME git RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/old-git)
naula_strict_warnings(fake_old_git)

function(naula_expect_exit name expected)
    cmake_parse_arguments(PARSE_ARGV 2 expect "" "HAS;HAS_ALSO;SEARCH_PATH" "ARGUMENTS")
    list(JOIN expect_ARGUMENTS "|" arguments)
    set(defines -DPROGRAM=$<TARGET_FILE:naula_command> -DARGUMENTS=${arguments} -DEXPECTED=${expected})
    foreach(member IN ITEMS HAS HAS_ALSO SEARCH_PATH)
        if(DEFINED expect_${member})
            list(APPEND defines "-D${member}=${expect_${member}}")
        endif()
    endforeach()
    add_test(NAME cli/${name} COMMAND ${CMAKE_COMMAND} ${defines} -P ${CMAKE_CURRENT_SOURCE_DIR}/expect_exit.cmake)
    set_tests_properties(cli/${name} PROPERTIES TIMEOUT 60 ENVIRONMENT "${NAULA_CLI_ENVIRONMENT}")
endfunction()

naula_expect_exit(an_old_git_is_refused_with_the_fix 1 ARGUMENTS check --repo ${CMAKE_CURRENT_BINARY_DIR}
    SEARCH_PATH $<TARGET_FILE_DIR:fake_old_git> HAS "git-too-old" HAS_ALSO "fix: install git 2.24 or newer")
naula_expect_exit(no_command_is_a_usage_error 2)
naula_expect_exit(an_unknown_command_is_a_usage_error 2 ARGUMENTS rename HAS "is not a naula command")
naula_expect_exit(an_unknown_option_is_a_usage_error 2 ARGUMENTS check --no-such-option)
naula_expect_exit(resolve_needs_an_id 2 ARGUMENTS resolve forge:1)
