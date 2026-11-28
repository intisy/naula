# The test build of the naula program: the same sources with a forge a test describes, and file urls allowed.
add_executable(naula_cli_test EXCLUDE_FROM_ALL ${NAULA_CLI_SOURCES} ${PROJECT_SOURCE_DIR}/src/cli/test_forge.c)
target_include_directories(naula_cli_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(naula_cli_test PRIVATE NAULA_TEST_HOOKS)
target_link_libraries(naula_cli_test PRIVATE naula_git naula naula_allocator $<$<PLATFORM_ID:Windows>:shell32>)
set_target_properties(naula_cli_test PROPERTIES OUTPUT_NAME naula RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/program)
naula_strict_warnings(naula_cli_test)

# Leaves an unfinished journal the way a process that died would, through the applier's test hooks.
add_executable(stopped_move EXCLUDE_FROM_ALL stopped_move.c)
target_include_directories(stopped_move PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(stopped_move PRIVATE NAULA_TEST_HOOKS)
target_link_libraries(stopped_move PRIVATE naula_test_hooks naula_allocator)
naula_strict_warnings(stopped_move)

add_executable(cli_test cli_test.c)
add_dependencies(cli_test naula_cli_test stopped_move)
target_include_directories(cli_test PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/test)
target_link_libraries(cli_test PRIVATE naula_git naula naula_allocator)
target_compile_definitions(cli_test PRIVATE NAULA_PROGRAM="$<TARGET_FILE:naula_cli_test>"
    NAULA_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}" NAULA_CMAKE="${CMAKE_COMMAND}"
    NAULA_STOPPED_MOVE="$<TARGET_FILE:stopped_move>")
naula_strict_warnings(cli_test)
# The driver builds paths and JSON in fixed buffers sized for its own fixtures; GCC cannot see those bounds.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(cli_test PRIVATE -Wno-format-truncation -Wno-restrict)
endif()

set(NAULA_CLI_HOME "${CMAKE_CURRENT_BINARY_DIR}/home")
file(MAKE_DIRECTORY "${NAULA_CLI_HOME}")
# Git for Windows stops at MAX_PATH inside nested checkouts unless told otherwise; other systems ignore the key.
# naula's own git children never see GIT_CONFIG_GLOBAL, so the same settings sit where git looks under HOME too.
foreach(config IN ITEMS gitconfig .gitconfig)
    file(WRITE "${NAULA_CLI_HOME}/${config}"
        "[core]\n\tlongpaths = true\n[init]\n\tdefaultBranch = main\n[user]\n\tname = Fixture\n\temail = fixture@example.invalid\n")
endforeach()
set(NAULA_CLI_ENVIRONMENT "HOME=${NAULA_CLI_HOME};USERPROFILE=${NAULA_CLI_HOME};XDG_CONFIG_HOME=${NAULA_CLI_HOME}/xdg;GIT_CONFIG_GLOBAL=${NAULA_CLI_HOME}/gitconfig;GIT_CONFIG_NOSYSTEM=1")

foreach(name IN ITEMS check_refuses_a_rebased_root check_warns_on_a_narrow_at_and_prints_the_line
        check_warns_on_a_url_dependency check_refuses_an_owner_repo_literal_outside_exempt_places
        siblings_clones_nested_paths siblings_refuses_a_copy sync_is_a_no_op_on_a_laid_out_root
        hold_adds_an_org_and_lays_it_out release_removes_an_orgs_links_but_never_a_dirty_clone
        clone_names_the_folder_by_twelve_hex siblings_refuses_a_link_in_its_own_siblings_folder
        siblings_refuses_a_link_a_dependency_tracks siblings_compares_paths_ignoring_case
        siblings_checks_every_commit_on_its_own_primary_before_cloning siblings_stops_a_fetch_past_its_byte_limit
        sync_refuses_while_a_listed_repo_is_unread clone_of_a_refused_repo_writes_nothing
        check_passes_the_adoption_of_an_existing_repo check_refuses_a_shallow_checkout
        check_fails_closed_past_its_finding_limit check_takes_zeros_as_no_previous check_ignores_an_inherited_git_dir
        check_prints_findings_as_json_with_the_verdict check_exempts_uses_only_as_a_step_key
        check_reads_a_repo_in_a_folder_named_outside_the_code_page sync_resumes_a_move_with_its_memory_bank
        check_counts_an_unheard_home_as_unverifiable sync_takes_the_lock_before_cloning
        init_and_sync_take_a_root_with_a_trailing_separator sync_skips_a_copy_a_home_disowns
        sync_refuses_a_copy_naula_linked sync_notes_an_empty_repo_as_unadopted
        sync_keeps_a_linked_copys_checkout_from_a_fork_of_its_id sync_refuses_a_fork_taking_a_checkout_naula_linked
        sync_refuses_an_empty_repo_naula_linked release_keeps_an_edited_one_line_rules_file
        sync_refuses_a_fork_of_our_name_in_another_held_account
        sync_moves_nothing_when_our_repo_beside_a_fork_of_its_name_is_made_a_copy
        sync_converges_after_a_home_refused_the_rename rename_is_refused_before_push_when_a_home_has_the_name
        rename_is_refused_for_a_reusable_workflow_repo transfer_moves_the_checkout_only_across_orgs
        planned_move_creates_the_new_home_and_pushes_every_ref retire_refuses_while_a_dependent_lacks_the_new_home
        retire_refuses_while_the_old_home_times_out retire_archives_only_after_the_listing_commit_is_pushed
        adopt_keeps_paths_and_repeats adopt_is_idempotent adopt_proposes_the_fork_exception_on_a_shared_root
        adopt_drops_claims_and_place new_creates_every_home_and_the_link add_writes_id_at_importname_and_commit)
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
