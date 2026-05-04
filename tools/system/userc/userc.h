#ifndef ONETOOL_TOOLS_SYSTEM_USERC_USERC_H
#define ONETOOL_TOOLS_SYSTEM_USERC_USERC_H

#include "../../../libs/TUI/tui.h"
#include <stddef.h>

#define USERC_MAX_USERS 256
#define USERC_MAX_GROUPS 64

typedef struct {
    char name[64];
    int uid;
    int gid;
    char home[128];
    char shell[64];
    char comment[128];
} userc_user_t;

typedef struct {
    userc_user_t users[USERC_MAX_USERS];
    int count;
} userc_list_t;

typedef struct {
    int selected;
    int scroll;
    int mode; // 0: list, 1: adding, 2: editing, 3: deleting
    char status[128];
    
    // form fields for add/edit
    char name_buf[64];
    char uid_buf[16];
    char gid_buf[16];
    char home_buf[128];
    char shell_buf[64];
    int field_idx;
} userc_view_t;

int uc(int argc, char *argv[]);
int userc_load_users(userc_list_t *list);
int userc_add_user(const char *name, const char *uid, const char *gid, const char *home, const char *shell);
int userc_del_user(const char *name);
void userc_draw_panel(const userc_list_t *list, userc_view_t *view, int x, int y, int width, int height);
int userc_handle_key(userc_list_t *list, userc_view_t *view, int key);

#endif
