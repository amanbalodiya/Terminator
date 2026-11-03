# Terminator — Tasks 1–9 source

This is a trimmed version of the V9 source. Task 11 custom script interpretation was removed from `src/script.c`, `src/executor.c`, `src/builtins.c`, and declarations. Task 10 Tab-completion implementation was not present in V9.

## Build (Ubuntu)

Dependencies: `build-essential`, `pkg-config`, `libx11-dev`, `libxft-dev`, `libfontconfig1-dev`, `libxrender-dev`.

```sh
make clean && make
./terminator
./terminator useProcess=True
```

## Included files

`src/` contains the shell engine, X11 GUI, history, jobs, parser, executor, and multiWatch implementation. `config/hotkeys.conf` is a sample **user-editable** hotkey configuration; the keyboard-event implementation lives in `src/main.c`. The GUI reads overrides from `~/.config/terminator/hotkeys.conf`, not directly from the packaged sample.

To customize, copy the sample to that location and restart Terminator. This keeps configurable shortcuts without recompiling.

## Scope note

Without the custom Task 11 interpreter, `bash script.sh`, `sh script.sh`, and executable scripts are delegated to external system executables (normal `execvp` behavior). The custom `source` and `.` builtins are removed. Shell variable assignments and expansions remain because other shell functionality depends on them.

This source has not been verified on a graphical X11 desktop; please test on Ubuntu before submission.
