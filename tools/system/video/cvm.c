#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
void print_fb_info(void) {
    int fd = open("/dev/fb0", O_RDONLY);
    if (fd < 0) {
        printf("video mode: vga text mode (no framebuffer detected)\n");
        return;
    }
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;

    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) == 0 &&
        ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == 0) {
        printf("video mode: %s\n", finfo.id);
        printf("current resolution: %dx%d (%d bpp)\n", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);
        printf("virtual resolution: %dx%d\n", vinfo.xres_virtual, vinfo.yres_virtual);
        // finding max resolution
        FILE *f = fopen("/sys/class/graphics/fb0/modes", "r");
        if (f) {
            char line[256];
            char max_mode[256] = "Unknown";
            while (fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\n")] = 0;
                strcpy(max_mode, line); 
            }
            printf("available/max modes (sysfs): %s\n", max_mode);
            fclose(f);
        }
    } else {
        printf("video mode: unknown (ioctl failed)\n");
    }

    close(fd);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    printf("cvm output:\n");
    printf("-------------------------------\n");
    print_fb_info();
    
    return 0;
}
