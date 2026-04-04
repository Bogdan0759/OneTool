CC ?= clang
CFLAGS ?= -O3
LDFLAGS ?= -static

TARGET := onetool
MAIN_OBJ := main.o
LASTMOD_SRC := tools/filesystem/lastmod.c
LASTMOD_OBJ := tools/filesystem/lastmod.o
REBOOT_SRC := tools/power/reboot.c
REBOOT_OBJ := tools/power/reboot.o
SHUTDOWN_SRC := tools/power/shutdown.c
SHUTDOWN_OBJ := tools/power/shutdown.o

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(MAIN_OBJ) $(LASTMOD_OBJ) $(REBOOT_OBJ) $(SHUTDOWN_OBJ)
	$(CC) $(MAIN_OBJ) $(LASTMOD_OBJ) $(REBOOT_OBJ) $(SHUTDOWN_OBJ) -o $@ $(LDFLAGS)

$(MAIN_OBJ): main.c
	$(CC) $(CFLAGS) -c $< -o $@

$(LASTMOD_OBJ): $(LASTMOD_SRC)
	$(CC) $(CFLAGS) -Dmain=lm -c $< -o $@

$(REBOOT_OBJ): $(REBOOT_SRC)
	$(CC) $(CFLAGS) -Dmain=rb -c $< -o $@

$(SHUTDOWN_OBJ): $(SHUTDOWN_SRC)
	$(CC) $(CFLAGS) -Dmain=sd -c $< -o $@


clean:
	rm -f $(TARGET) $(MAIN_OBJ) $(LASTMOD_OBJ) $(REBOOT_OBJ) $(SHUTDOWN_OBJ)
