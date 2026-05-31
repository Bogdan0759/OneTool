#define _GNU_SOURCE
#include "wayland_bridge.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

static void bridge_log(const char *debug_file, const char *fmt, ...) {
    va_list ap;
    va_list file_ap;
    char time_buf[32];
    time_t now = time(NULL);
    struct tm tm_now;

    localtime_r(&now, &tm_now);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_now);

    fprintf(stderr, "[wayland_bridge %s pid=%d] ", time_buf, (int)getpid());
    va_start(ap, fmt);
    va_copy(file_ap, ap);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);

    if (debug_file != NULL) {
        FILE *f = fopen(debug_file, "a");
        if (f != NULL) {
            fprintf(f, "[wayland_bridge %s pid=%d] ", time_buf, (int)getpid());
            vfprintf(f, fmt, file_ap);
            fputc('\n', f);
            fclose(f);
        }
    }
    va_end(file_ap);
}

static int reset_debug_file(const char *debug_file) {
    if (debug_file == NULL) return 0;
    FILE *f = fopen(debug_file, "w");
    if (f == NULL) return -1;
    fclose(f);
    return 0;
}

static void log_wait_status(const char *debug_file, const char *name, int status) {
    if (WIFEXITED(status)) {
        bridge_log(debug_file, "%s exited with status %d", name, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        bridge_log(debug_file, "%s terminated by signal %d", name, WTERMSIG(status));
    } else if (WIFSTOPPED(status)) {
        bridge_log(debug_file, "%s stopped by signal %d", name, WSTOPSIG(status));
    } else {
        bridge_log(debug_file, "%s changed state: status=0x%x", name, status);
    }
}

int wayland_bridge_main(int argc, char *argv[]) {
    const char *socket_path = "/tmp/swm.sock";
    const char *display_name = "wayland-0";
    const char *debug_file = NULL;
    const char *app_to_run = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "--display") == 0 && i + 1 < argc) {
            display_name = argv[++i];
        } else if (strcmp(argv[i], "--debug") == 0 && i + 1 < argc) {
            debug_file = argv[++i];
        } else if (strcmp(argv[i], "--run") == 0 && i + 1 < argc) {
            app_to_run = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("wayland_bridge - SWM Wayland proxy launcher\n");
            printf("usage: onetool wayland_bridge [--socket /path/to/swm.sock] [--display wayland-0] [--debug /path/to/log] [app_to_run]\n");
            return 0;
        } else if (argv[i][0] != '-') {
            app_to_run = argv[i];
        } else {
            fprintf(stderr, "[wayland_bridge] unknown argument: %s\n", argv[i]);
        }
    }

    if (reset_debug_file(debug_file) != 0) {
        fprintf(stderr, "[wayland_bridge] failed to open debug file %s: %s\n", debug_file, strerror(errno));
    }

    bridge_log(debug_file, "starting argc=%d argv0=%s", argc, argv[0] ? argv[0] : "(null)");
    bridge_log(debug_file, "display=%s swm_socket=%s", display_name, socket_path);
    if (debug_file) {
        bridge_log(debug_file, "debug_file=%s", debug_file);
    }
    if (app_to_run != NULL) {
        bridge_log(debug_file, "app command=%s", app_to_run);
    } else {
        bridge_log(debug_file, "no app command, wlbridge will stay in foreground");
    }

    pid_t bridge_pid = fork();
    if (bridge_pid < 0) {
        bridge_log(debug_file, "fork wlbridge failed: %s", strerror(errno));
        return 1;
    }

    if (bridge_pid == 0) {
        char display_arg[64];
        snprintf(display_arg, sizeof(display_arg), "--display=%s", display_name);
        char socket_arg[128];
        snprintf(socket_arg, sizeof(socket_arg), "--socket=%s", socket_path);
        char debug_arg[256];
        if (debug_file) {
            snprintf(debug_arg, sizeof(debug_arg), "--debug=%s", debug_file);
            setenv("WLBRIDGE_DEBUG_APPEND", "1", 1);
        }

        char wlbridge_path[512] = {0};
        const char *last_slash = strrchr(argv[0], '/');
        if (last_slash != NULL) {
            size_t dir_len = (size_t)(last_slash - argv[0]);
            if (dir_len < sizeof(wlbridge_path) - 10) {
                memcpy(wlbridge_path, argv[0], dir_len);
                strcpy(wlbridge_path + dir_len, "/wlbridge");
            }
        }

        char *args[6];
        int arg_idx = 0;
        args[arg_idx++] = (wlbridge_path[0] != '\0') ? wlbridge_path : "./wlbridge";
        args[arg_idx++] = display_arg;
        args[arg_idx++] = socket_arg;
        if (debug_file) {
            args[arg_idx++] = debug_arg;
        }
        args[arg_idx] = NULL;

        bridge_log(debug_file, "exec wlbridge path=%s display_arg=%s socket_arg=%s debug=%s",
                   args[0], display_arg, socket_arg, debug_file ? debug_file : "off");
        execvp(args[0], args);
        bridge_log(debug_file, "exec failed path=%s: %s", args[0], strerror(errno));

        if (wlbridge_path[0] != '\0') {
            args[0] = "./wlbridge";
            bridge_log(debug_file, "exec fallback path=%s", args[0]);
            execvp(args[0], args);
            bridge_log(debug_file, "exec fallback failed path=%s: %s", args[0], strerror(errno));
        }
        args[0] = "wlbridge";
        bridge_log(debug_file, "exec fallback path=%s", args[0]);
        execvp(args[0], args);

        bridge_log(debug_file, "exec wlbridge failed: %s", strerror(errno));
        exit(1);
    }

    bridge_log(debug_file, "wlbridge forked pid=%d", (int)bridge_pid);
    usleep(250000);

    int status;
    pid_t early = waitpid(bridge_pid, &status, WNOHANG);
    if (early == bridge_pid) {
        log_wait_status(debug_file, "wlbridge", status);
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    } else if (early < 0) {
        bridge_log(debug_file, "waitpid wlbridge failed after startup: %s", strerror(errno));
        return 1;
    }
    bridge_log(debug_file, "wlbridge still running after startup delay");

    if (app_to_run != NULL && app_to_run[0] != '\0') {
        bridge_log(debug_file, "starting app: %s", app_to_run);
        pid_t app_pid = fork();
        if (app_pid < 0) {
            bridge_log(debug_file, "fork app failed: %s", strerror(errno));
            kill(bridge_pid, SIGTERM);
            waitpid(bridge_pid, NULL, 0);
            return 1;
        }

        if (app_pid == 0) {
            setenv("WAYLAND_DISPLAY", display_name, 1);
            if (getenv("XDG_RUNTIME_DIR") == NULL) {
                setenv("XDG_RUNTIME_DIR", "/tmp", 1);
            }

            bridge_log(debug_file, "exec app via /bin/sh -c, WAYLAND_DISPLAY=%s XDG_RUNTIME_DIR=%s",
                       getenv("WAYLAND_DISPLAY"), getenv("XDG_RUNTIME_DIR"));
            char *args[] = { "/bin/sh", "-c", (char *)app_to_run, NULL };
            execv(args[0], args);
            bridge_log(debug_file, "exec app failed: %s", strerror(errno));
            exit(1);
        }

        bridge_log(debug_file, "app forked pid=%d", (int)app_pid);
        while (1) {
            pid_t p = waitpid(app_pid, &status, WNOHANG);
            if (p == app_pid) {
                log_wait_status(debug_file, "application", status);
                bridge_log(debug_file, "stopping wlbridge pid=%d after application exit", (int)bridge_pid);
                kill(bridge_pid, SIGTERM);
                waitpid(bridge_pid, NULL, 0);
                break;
            } else if (p < 0) {
                bridge_log(debug_file, "waitpid application failed: %s", strerror(errno));
                kill(bridge_pid, SIGTERM);
                waitpid(bridge_pid, NULL, 0);
                break;
            }

            p = waitpid(bridge_pid, &status, WNOHANG);
            if (p == bridge_pid) {
                log_wait_status(debug_file, "wlbridge", status);
                bridge_log(debug_file, "stopping app pid=%d because wlbridge stopped", (int)app_pid);
                kill(app_pid, SIGTERM);
                waitpid(app_pid, NULL, 0);
                break;
            } else if (p < 0) {
                bridge_log(debug_file, "waitpid wlbridge failed: %s", strerror(errno));
                kill(app_pid, SIGTERM);
                waitpid(app_pid, NULL, 0);
                break;
            }

            usleep(20000); // 20ms
        }
    } else {
        bridge_log(debug_file, "wlbridge running pid=%d, waiting until it exits", (int)bridge_pid);
        waitpid(bridge_pid, &status, 0);
        log_wait_status(debug_file, "wlbridge", status);
    }

    bridge_log(debug_file, "done");
    return 0;
}

int wb(int argc, char *argv[]) {
    return wayland_bridge_main(argc, argv);
}
