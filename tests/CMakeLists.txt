# 回归测试：每条都对应 README「已修复问题」里的一项行为契约。
# 全部通过 = 本轮修复的 P0 行为没有回退。

set(PM_TEST_BIN ${CMAKE_BINARY_DIR}/${PROJECT_NAME})
set(PM_TEST_SCRIPTS
        test_config_defaults
        test_check_interval_clamp
        test_restart_backoff
        test_restart_budget_reset
        test_zero_restarts
        test_graceful_shutdown
        test_name_alias
        test_exec_failure
        test_exit_status_logged
        test_no_zombie
        test_pidfile_lock
        test_orphan_prevention
)

foreach (name ${PM_TEST_SCRIPTS})
    add_test(
            NAME ${name}
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/${name}.sh" "${PM_TEST_BIN}"
    )
    set_tests_properties(${name} PROPERTIES TIMEOUT 90)
endforeach ()
