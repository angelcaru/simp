#include <stdlib.h>
#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#include "nob.h"

typedef enum {
    TARGET_LINUX,
    TARGET_LINUX_HOTRELOAD,
    TARGET_WINDOWS,
    COUNT_TARGETS,
} Target;

const char *target_as_cstr(Target target) {
    static_assert(COUNT_TARGETS == 3, "Please update after adding a new target");
    switch (target) {
        case TARGET_LINUX: return "linux";
        case TARGET_LINUX_HOTRELOAD: return "linux-hotreload";
        case TARGET_WINDOWS: return "windows";
        default: UNREACHABLE("invalid target");
    }
}

#ifdef _WIN32
Target default_target = TARGET_WINDOWS;
#else
Target default_target = TARGET_LINUX;
#endif

void list_targets(FILE *stream) {
    for (Target target = 0; target < COUNT_TARGETS; target++) {
        fprintf(stream, "      %s", target_as_cstr(target));
        if (target == default_target) {
            fprintf(stream, " (default)");
        }
        fprintf(stream, "\n");
    }
}

void usage(FILE *stream, const char *program_name) {
    fprintf(stream, "Usage: %s [OPTIONS]\n", program_name);
    fprintf(stream, "  OPTIONS:\n");
    fprintf(stream, "    -h, --help - Print this help message\n");
    fprintf(stream, "    -r - Run app after building\n");
    static_assert(COUNT_TARGETS == 3, "Please update usage after adding a new target");
    fprintf(stream, "    -t <target> - Build for a specific target. Possible targets include:\n");
    list_targets(stream);
    fprintf(stream, "    -t list - Print the above list of targets and exit\n");
    fprintf(stream, "    If this option is not provided, the default target is `%s`\n", target_as_cstr(default_target));
}

void common_cflags(Cmd *cmd) {
    cmd_append(cmd, "-std=gnu11");
    cmd_append(cmd, "-Wall", "-Wextra", "-g");
    cmd_append(cmd, "-I.", "-I./raylib/", "-I./clay/", "-I./tinyfiledialogs/");
}

int main(int argc, char **argv) {
    NOB_GO_REBUILD_URSELF(argc, argv);

    const char *program_name = nob_shift(argv, argc);

    bool run = false;
    Target target = default_target;
    while (argc > 0) {
        const char *arg = shift(argv, argc);
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout, program_name);
            return 0;
        } else if (strcmp(arg, "-t") == 0) {
            if (argc == 0) {
                usage(stderr, program_name);
                nob_log(ERROR, "-t flag requires an argument");
            }
            const char *target_name = shift(argv, argc);
            static_assert(COUNT_TARGETS == 3, "Please update the -t flag when adding a new target");
            if (strcmp(target_name, "linux") == 0) {
                target = TARGET_LINUX;
            } else if (strcmp(target_name, "linux-hotreload") == 0 || strcmp(target_name, "lh") == 0) {
                target = TARGET_LINUX_HOTRELOAD;
            } else if (strcmp(target_name, "windows") == 0) {
                target = TARGET_WINDOWS;
            } else if (strcmp(target_name, "list") == 0) {
                list_targets(stdout);
                return 0;
            } else {
                usage(stderr, program_name);
                nob_log(ERROR, "unknown target %s", target_name);
                return 1;
            }
        } else if (strcmp(arg, "-r") == 0) {
            run = true;
        } else {
            usage(stderr, program_name);
            nob_log(ERROR, "unknown flag %s", arg);
            return 1;
        }
    }

    if (!mkdir_if_not_exists("./build/")) return 1;

    Cmd cmd = {0};
    static_assert(COUNT_TARGETS == 3, "Please update this `switch` statement when adding a new target");
    switch (target) {
        case TARGET_LINUX:
#ifdef _WIN32
            cmd_append(&cmd, "wsl", "gcc");
#else
            cmd_append(&cmd, "cc");
#endif
            common_cflags(&cmd);
            cmd_append(&cmd, "-o", "./build/main");
            cmd_append(&cmd, "./src/main.c", "./src/app.c", "./tinyfiledialogs/tinyfiledialogs.c");
            cmd_append(&cmd, "./raylib/libraylib.a", "-lm");
            break;
        case TARGET_LINUX_HOTRELOAD:
#ifdef _WIN32
            nob_log(ERROR, "Cannot compile for `%s` on Windows", target_as_cstr(target));
            return 1;
#else
            cmd_append(&cmd, "cc");
            common_cflags(&cmd);
            cmd_append(&cmd, "-o", "./build/main");
            cmd_append(&cmd, "./src/main.c");
            cmd_append(&cmd, "-L./raylib/", "-l:libraylib.so.550", "-lm");
            cmd_append(&cmd, "-DHOTRELOAD");
            if (!nob_cmd_run_sync_and_reset(&cmd)) return 1;

            cmd_append(&cmd, "cc");
            common_cflags(&cmd);
            cmd_append(&cmd, "-shared", "-fPIC");
            cmd_append(&cmd, "-o", "./build/libapp.so");
            cmd_append(&cmd, "./src/app.c", "./tinyfiledialogs/tinyfiledialogs.c");
            cmd_append(&cmd, "-L./raylib/", "-l:libraylib.so.550", "-lm");
            cmd_append(&cmd, "-DHOTRELOAD");
#endif // _WIN32
            break;
        case TARGET_WINDOWS:
            cmd_append(&cmd, "x86_64-w64-mingw32-gcc");
            common_cflags(&cmd);
            cmd_append(&cmd, "-o", "./build/main.exe");
            cmd_append(&cmd, "./src/main.c", "./src/app.c", "./tinyfiledialogs/tinyfiledialogs.c");
            cmd_append(&cmd, "-L./raylib/", "-lraylib.win", "-lm");
            cmd_append(&cmd, "-lwinmm", "-lgdi32", "-lcomdlg32", "-lole32");
            break;
        default:
            UNREACHABLE("invalid target");
    }
    if (!cmd_run_sync_and_reset(&cmd)) return 1;

    static_assert(COUNT_TARGETS == 3, "Please update this `switch` statement when adding a new target");
    if (run) {
        switch (target) {
            case TARGET_LINUX_HOTRELOAD:
#ifdef _WIN32
                UNREACHABLE("should have failed earlier");
#else
                setenv("LD_LIBRARY_PATH", "./raylib/", 1);
#endif // _WIN32
                // fallthrough
            case TARGET_LINUX:
                cmd_append(&cmd, "./build/main");
                break;
            case TARGET_WINDOWS:
                cmd_append(&cmd, "wine", "./build/main.exe");
                break;
            default:
                UNREACHABLE("invalid target");
        }
        if (!cmd_run_sync_and_reset(&cmd)) return 1;
        return 0;
    }
}
