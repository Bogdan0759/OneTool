#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config/tool_registry.h"

int lm(int argc, char *argv[]);
int ex(int argc, char *argv[]);
int fi(int argc, char *argv[]);
int ga(int argc, char *argv[]);
int dn(int argc, char *argv[]);
int pg(int argc, char *argv[]);
int lk(int argc, char *argv[]);
int lp(int argc, char *argv[]);
int rb(int argc, char *argv[]);
int sd(int argc, char *argv[]);
int tm(int argc, char *argv[]);
int cf(int argc, char *argv[]);
int tui_main(const char *onetool_argv0);
const char onetool_version[] = "0.7.1";

const struct onetool_tool onetool_builtin_tools[] = {
    {"lastmod", lm, "print the last modification time of a file",
     ONETOOL_ARGV0_TOOL_NAME},
    {"exec", ex, "execute file (optional: -i interpreter)",
     ONETOOL_ARGV0_TOOL_NAME},
    {"fsinfo", fi, "print filesystem info", ONETOOL_ARGV0_TOOL_NAME},
    {"gapi_supported", ga, "check available graphics API",
     ONETOOL_ARGV0_TOOL_NAME},
    {"down", dn, "HTTP downloader (curl-like)", ONETOOL_ARGV0_TOOL_NAME},
    {"ping", pg, "ICMP ping with stats", ONETOOL_ARGV0_TOOL_NAME},
    {"lmake", lk, "run bundled lmake build tool", ONETOOL_ARGV0_BINARY_PATH},
    {"lpack", lp, "pack lua script into ELF runtime", ONETOOL_ARGV0_TOOL_NAME},
    {"reboot", rb, "reboot the system (optional: -t seconds)",
     ONETOOL_ARGV0_TOOL_NAME},
    {"shutdown", sd, "power off the system (optional: -t seconds)",
     ONETOOL_ARGV0_TOOL_NAME},
    {"taskmng", tm, "interactive task manager with process control",
     ONETOOL_ARGV0_TOOL_NAME},
    {"configure", cf, "configure basic system environment",
     ONETOOL_ARGV0_TOOL_NAME},
};

const int onetool_builtin_tool_count =
    sizeof(onetool_builtin_tools) / sizeof(onetool_builtin_tools[0]);

static void print_tool_list(const struct onetool_tool *tools, int count) {
  for (int i = 0; i < count; i++) {
    printf("  %s - %s\n", tools[i].name, tools[i].description);
  }
}

int onetool_total_tool_count(void) {
  return onetool_builtin_tool_count + onetool_extra_tool_count;
}

const struct onetool_tool *onetool_get_tool_by_index(int index) {
  if (index < 0) {
    return NULL;
  }
  if (index < onetool_builtin_tool_count) {
    return &onetool_builtin_tools[index];
  }

  index -= onetool_builtin_tool_count;
  if (index < onetool_extra_tool_count) {
    return &onetool_extra_tools[index];
  }

  return NULL;
}

const struct onetool_tool *onetool_find_tool(const char *name) {
  for (int i = 0; i < onetool_builtin_tool_count; i++) {
    if (strcmp(name, onetool_builtin_tools[i].name) == 0) {
      return &onetool_builtin_tools[i];
    }
  }

  for (int i = 0; i < onetool_extra_tool_count; i++) {
    if (strcmp(name, onetool_extra_tools[i].name) == 0) {
      return &onetool_extra_tools[i];
    }
  }

  return NULL;
}

int onetool_run_tool(const struct onetool_tool *tool, int argc, char *argv[],
                     const char *onetool_argv0) {
  if (tool->argv0_mode == ONETOOL_ARGV0_BINARY_PATH) {
    argv[0] = (char *)onetool_argv0;
  } else {
    argv[0] = (char *)tool->name;
  }

  return tool->entry(argc, argv);
}

void onetool_show_help(void) {
  printf("OneTool %s\n", onetool_version);
  printf("usage: %s <tool> [args] -to fd/path\n", "onetool");
  printf("\n");
  printf("available tools:\n");
  print_tool_list(onetool_builtin_tools, onetool_builtin_tool_count);
  if (onetool_extra_tool_count > 0) {
    print_tool_list(onetool_extra_tools, onetool_extra_tool_count);
  }
  printf("\n");
  printf("global options:\n");
  printf("  -to fd/path - redirect stdout and stderr to file\n");
  printf("  tui        - launch the TUI\n");
}

int main(int argc, char *argv[]) {
  char *tool_argv[argc + 1];
  int tool_argc = 1;
  const char *to_target = NULL;

  if (argc < 2) {
    onetool_show_help();
    return 1;
  }
  if (strcmp(argv[1], "tui") == 0) {
    return tui_main(argv[0]);
  }

  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "-to") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value for -to\n");
        return 1;
      }
      to_target = argv[++i];
      continue;
    }

    tool_argv[tool_argc++] = argv[i];
  }
  tool_argv[tool_argc] = NULL;

  if (to_target != NULL && strcmp(to_target, "fd") != 0) {
    int out_fd = open(to_target, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
      fprintf(stderr, "open(%s) failed: %s\n", to_target, strerror(errno));
      return 1;
    }
    if (dup2(out_fd, STDOUT_FILENO) < 0) {
      fprintf(stderr, "dup2 failed: %s\n", strerror(errno));
      close(out_fd);
      return 1;
    }
    if (dup2(out_fd, STDERR_FILENO) < 0) {
      fprintf(stderr, "dup2 failed: %s\n", strerror(errno));
      close(out_fd);
      return 1;
    }
    close(out_fd);
  }

  const struct onetool_tool *tool = onetool_find_tool(argv[1]);
  if (tool != NULL) {
    return onetool_run_tool(tool, tool_argc, tool_argv, argv[0]);
  }

  if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0 ||
      strcmp(argv[1], "help") == 0 || strcmp(argv[1], "list") == 0) {
    onetool_show_help();
    return 0;
  }

  fprintf(stderr, "unknown tool: %s\n", argv[1]);
  fprintf(stderr, "try: %s --help\n", argv[0]);
  return 1;
}
