#ifndef ONETOOL_TOOL_REGISTRY_H
#define ONETOOL_TOOL_REGISTRY_H

typedef int (*onetool_entry_fn)(int argc, char *argv[]);

enum onetool_argv0_mode {
    ONETOOL_ARGV0_TOOL_NAME = 0,
    ONETOOL_ARGV0_BINARY_PATH = 1,
};

struct onetool_tool {
    const char *name;
    onetool_entry_fn entry;
    const char *description;
    enum onetool_argv0_mode argv0_mode;
};

extern const struct onetool_tool *onetool_extra_tools;
extern const int onetool_extra_tool_count;

#endif
