#include <sys/ioctl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

void size_setup(void) {
    struct winsize w;
    char lines[16];
    char cols[16];

    // geting terminal size
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        if (w.ws_row > 0 && w.ws_col > 0) {
            snprintf(lines, sizeof(lines), "%d", w.ws_row);
            snprintf(cols, sizeof(cols), "%d", w.ws_col);
            setenv("LINES", lines, 1);
            setenv("COLUMNS", cols, 1);
            return;
        }
    }
    // fallback
    setenv("LINES", "24", 0);
    setenv("COLUMNS", "80", 0);
}
