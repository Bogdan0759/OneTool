// lastmod program. print the last modification time of a file
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <string.h>
// you can redirect the output of this program by argument

// example using: lastmod fd /path/to/file
// or file: lastmod /dev/tty1 /path/to/file
int main(int argc, char *argv[]) {
    
    if (argc != 3) {
        fprintf(stderr, "invalid usage %s\n", argv[0]);
        return 1;
    }
    const char *output_file = argv[1];
    const char *input_file = argv[2];
    struct stat file_stat;
    if (stat(input_file, &file_stat) == -1) {
        perror("stat");
        return 1;
    }
    time_t last_mod_time = file_stat.st_mtime;
    if (strcmp(output_file, "fd") == 0) {
        printf("This tool is the part of OneTool project that published in MPL-2.0 license\n");

        printf("last modification time of %s is: %s", input_file, ctime(&last_mod_time));
    } else {
        FILE *out_fp = fopen(output_file, "w");
        if (out_fp == NULL) {
            perror("fopen");
            return 1;
        }
        fprintf(out_fp, "This tool is the part of OneTool project that published in MPL-2.0 license\n");
        fprintf(out_fp, "last modification time of %s is: %s", input_file, ctime(&last_mod_time));
        fclose(out_fp);
    }
    return 0;
}
