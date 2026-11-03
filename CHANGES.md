# Cleanup changes from V9
- Removed `src/script.c` (Task 11 custom script interpreter).
- Removed the custom script interception from `src/executor.c`. External scripts now run normally via `execvp`.
- Removed the custom `source` and `.` builtins from `src/builtins.c`.
- Removed obsolete `src/stubs.c` and Task 11 declarations/helper.
- Removed outdated V9 test notes and replaced outdated README.
- Task 10 Tab completion: no implementation existed in V9 `src/main.c`; no feature code to remove.
- Kept `config/hotkeys.conf`: it is a sample for configurable shortcuts, which Task 7 requires.
- Kept `src/main_cli.c` and `shell_test` as an optional CLI test harness.
