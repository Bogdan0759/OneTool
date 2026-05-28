#define _GNU_SOURCE
#include "wayland_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

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
        }
    }

    printf("[wayland_bridge] starting\n");
    printf("[wayland_bridge] display %s, swm socket: %s\n", display_name, socket_path);
    if (debug_file) {
        printf("[wayland_bridge] file log: %s\n", debug_file);
    }

    pid_t bridge_pid = fork();
    if (bridge_pid < 0) {
        perror("fork");
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

        execvp(args[0], args);

        // Fallbacks
        if (wlbridge_path[0] != '\0') {
            args[0] = "./wlbridge";
            execvp(args[0], args);
        }
        args[0] = "wlbridge";
        execvp(args[0], args);

        perror("[wayland_bridge] exec wlbridge failed");
        exit(1);
    }

    usleep(250000);

    if (app_to_run != NULL && app_to_run[0] != '\0') {
        printf("[wayland_bridge] program: %s\n", app_to_run);
        pid_t app_pid = fork();
        if (app_pid < 0) {
            perror("fork app");
            return 1;
        }

        if (app_pid == 0) {
            setenv("WAYLAND_DISPLAY", display_name, 1);
            if (getenv("XDG_RUNTIME_DIR") == NULL) {
                setenv("XDG_RUNTIME_DIR", "/tmp", 1);
            }

            char *args[] = { "/bin/sh", "-c", (char *)app_to_run, NULL };
            execv(args[0], args);
            perror("exec app");
            exit(1);
        }

        int status;
        while (1) {
            pid_t p = waitpid(app_pid, &status, WNOHANG);
            if (p == app_pid) {
                if (WIFEXITED(status)) {
                    printf("[wayland_bridge] application exited normally with status %d.\n", WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                    printf("[wayland_bridge] application terminated by signal %d.\n", WTERMSIG(status));
                } else {
                    printf("[wayland_bridge] application exited.\n");
                }
                kill(bridge_pid, SIGTERM);
                waitpid(bridge_pid, NULL, 0);
                break;
            }

            p = waitpid(bridge_pid, &status, WNOHANG);
            if (p == bridge_pid) {
                if (WIFEXITED(status)) {
                    printf("[wayland_bridge] wlbridge exited unexpectedly with status %d.\n", WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                    printf("[wayland_bridge] wlbridge crashed/terminated by signal %d.\n", WTERMSIG(status));
                } else {
                    printf("[wayland_bridge] wlbridge exited unexpectedly.\n");
                }
                kill(app_pid, SIGTERM);
                waitpid(app_pid, NULL, 0);
                break;
            }

            usleep(20000); // 20ms
        }
    } else {
        printf("[wayland_bridge] wlbridge running in background (PID: %d). Press Ctrl+C or kill to stop.\n", bridge_pid);
        int status;
        waitpid(bridge_pid, &status, 0);
        if (WIFSIGNALED(status)) {
            printf("[wayland_bridge] wlbridge crashed/terminated by signal %d.\n", WTERMSIG(status));
        } else {
            printf("[wayland_bridge] wlbridge exited with status %d.\n", WEXITSTATUS(status));
        }
    }

    return 0;
}

int wb(int argc, char *argv[]) {
    return wayland_bridge_main(argc, argv);
}

