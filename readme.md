# OneTool

OneTool - a single static Linux binary that contains multiple built-in tools

The long-term idea is simple:
one executable, one CLI entrypoint, many capabilities implemented from scratch inside the project.


**Warning: Some parts of the code were written by AI because I don't really understand HTTP**
## status

implemented tools:

- `lastmod` - prints the last modification time of a file
- `reboot` - reboot the system
- `shutdown` - shut down the system
- `exec` - execute file
- `down` - HTTP downloader
- `ping` - ICMP ping with stats
- `lmake` - run bundled lmake build tool
- `lpack` - pack lua script into ELF runtime
- `mlink` - minimalistic ELF64 x86-64 linker
- `taskmng` - TUI task manager with process control
- `gapi_supported` - check available graphics API
- `fsinfo` - filesystem info
- `yap` - YAP language interpreter
- `userc` - interactive user control (add/del/edit users)
- `nsetup` - simple network setup via DHCP

framework parts:

- `libs/net` - network library for URL parsing, HTTP and transport
- `libs/TUI` - TUI library

## build

requirement:
- `clang`
- `make`

build:

```bash
make
```

delete binaries:

```bash
make clean
```

### add custom tools

You can bundle extra C tools into `onetool` at build time without editing
`main.c`.

Add a line to [`config/extra_tools.manifest`](./config/extra_tools.manifest)
Run `make`

Manifest format:

```text
name|source|description|argv0_mode|extra_cflags
```

Example:

```text
yap|mofl/languages/yap/yap.c|YAP language interpreter|tool|
```

Notes:
- the source file must expose a normal `main(int argc, char **argv)`
- the build renames that `main` automatically during compilation
- `argv0_mode` can be `tool` or `onetool`

## tui

You can launch TUI by:

```bash
./onetool tui
```

Themes and built-in tool forms are embedded directly into the binary by [tui.c](./tui.c).

Inside TUI:

- `Enter` opens the selected tool form
- `N` opens global settings
- `T` quickly cycles themes
- inside a tool form, `Enter` runs the tool
- settings let you change the launch path for tools and edit theme colors live

If a tool does not have a built-in TUI form, OneTool falls back to a
generic screen with `Extra args`, so all tools can still be launched.

## mlink

`mlink` links freestanding ELF64 x86-64 `.o` files into a static executable.
It is intentionally small: good for tiny syscall-only programs, kernels,
boot experiments, and OneTool-style low-level demos.

Example:

```bash
./onetool mlink start.o util.o -o app.out -Map link.map
```

Supported MVP features:
- ELF64 little-endian x86-64 relocatable objects
- simple `.a` archive member selection
- `.text`, `.rodata`, `.data`, `.bss` and common symbols
- `R_X86_64_64`, `R_X86_64_PC32`, `R_X86_64_PLT32`, `R_X86_64_32`, `R_X86_64_32S`
- custom entry symbol and base address
- map output, symbol dump and dry-run mode

It does not try to replace system `ld`: no libc startup, dynamic linking,
linker scripts or shared libraries yet.


## How version counting works
x.r.f
x = when r > 9
r = global update or new tool
f = small update or bug fix

## License

### This project contains multiple licensing schemes:

#### - mofl/ -  BSD 2-Clause
#### - other - MPL 2.0
