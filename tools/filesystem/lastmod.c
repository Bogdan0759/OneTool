// lastmod program. print the last modification time of a file
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
int main(int argc, char *argv[]) {
    printf("This tool is the part of OneTool project that published in MPL-2.0 license\n");
    
    if (argc != 2) {
        fprintf(stderr, "usage: %s <path>\n", argv[0]);
        return 1;
    }
    const char *input_file = argv[1];
    struct stat file_stat;
    if (stat(input_file, &file_stat) == -1) {
        perror("stat");
        return 1;
    }
    time_t last_mod_time = file_stat.st_mtime;
    printf("last modification time of %s is: %s", input_file, ctime(&last_mod_time));
    return 0;
}
