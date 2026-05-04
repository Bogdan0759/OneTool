#include "userc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int userc_load_users(userc_list_t *list) {
    FILE *fp = fopen("/etc/passwd", "r");
    if (!fp) return -1;

    list->count = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp) && list->count < USERC_MAX_USERS) {
        userc_user_t *u = &list->users[list->count];
        char *token;
        char *rest = line;

        // name:password:uid:gid:comment:home:shell
        if ((token = strsep(&rest, ":"))) strncpy(u->name, token, sizeof(u->name)-1);
        strsep(&rest, ":"); // skip password
        if ((token = strsep(&rest, ":"))) u->uid = atoi(token);
        if ((token = strsep(&rest, ":"))) u->gid = atoi(token);
        if ((token = strsep(&rest, ":"))) strncpy(u->comment, token, sizeof(u->comment)-1);
        if ((token = strsep(&rest, ":"))) strncpy(u->home, token, sizeof(u->home)-1);
        if ((token = strsep(&rest, ":"))) {
            strncpy(u->shell, token, sizeof(u->shell)-1);
            char *nl = strchr(u->shell, '\n');
            if (nl) *nl = '\0';
        }
        list->count++;
    }
    fclose(fp);
    return 0;
}

int userc_add_user(const char *name, const char *uid, const char *gid, const char *home, const char *shell) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "useradd %s %s %s %s %s %s %s %s %s",
             (uid && uid[0]) ? "-u" : "", (uid && uid[0]) ? uid : "",
             (gid && gid[0]) ? "-g" : "", (gid && gid[0]) ? gid : "",
             (home && home[0]) ? "-d" : "", (home && home[0]) ? home : "",
             (shell && shell[0]) ? "-s" : "", (shell && shell[0]) ? shell : "",
             name);
    return system(cmd);
}

int userc_del_user(const char *name) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "userdel -r %s", name);
    return system(cmd);
}
