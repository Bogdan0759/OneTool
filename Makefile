CC ?= clang
CFLAGS ?= -O3
LDFLAGS ?= -static

TARGET := onetool
MAIN_OBJ := main.o
LASTMOD_SRC := tools/filesystem/lastmod.c
LASTMOD_OBJ := tools/filesystem/lastmod.o

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(MAIN_OBJ) $(LASTMOD_OBJ)
	$(CC) $(MAIN_OBJ) $(LASTMOD_OBJ) -o $@ $(LDFLAGS)

$(MAIN_OBJ): main.c
	$(CC) $(CFLAGS) -c $< -o $@

$(LASTMOD_OBJ): $(LASTMOD_SRC)
	$(CC) $(CFLAGS) -Dmain=lm -c $< -o $@


clean:
	rm -f $(TARGET) $(MAIN_OBJ) $(LASTMOD_OBJ)
