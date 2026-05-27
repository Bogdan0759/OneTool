# OneTool

OneTool - a single static Linux binary that contains multiple built-in tools

The long-term idea is simple:
one executable, one CLI entrypoint, many capabilities implemented from scratch inside the project.


**Warning: Some parts of the code were written by AI because I don't really understand HTTP**
## status

implemented tools:

- `lastmod` - prints the last modification time of a file (lastmod = last modification)
- `reboot` - reboot the system
- `shutdown` - shut down the system
- `exec` - execute file (exec = execute)
- `down` - HTTP downloader (down = downloader)
- `ping` - ICMP ping with stats
- `lmake` - run bundled lmake build tool
- `lpack` - pack lua script into ELF runtime (lpack = lua packager)
- `mlink` - minimalistic ELF64 x86-64 linker (mlink = minimalistic linker)
- `taskmng` - TUI task manager with process control (taskmng = task manager)
- `gapi_supported` - check available graphics API (gapi_supported = graphics api supported)
- `srapi_demo` - render a test frame with SRAPI
- `srvid2mp4` - convert .srvid screen recordings to mp4/webm via ffmpeg
- `fsinfo` - filesystem info (fsinfo = filesystem info)
- `fsize` - print file size (fsize = file size)
- `ls` - list files (ls = list)
- `cat` - print files content
- `free` - show memory usage
- `yap` - YAP language interpreter
- `userc` - interactive user control (add/del/edit users) (userc = user control)
- `nsetup` - simple network setup via DHCP (nsetup = network setup)
- `size` - prints elf sections size
- `nm` - prints elf symbols
- `click` - SRAPI click-the-cube minigame (15s round)
- `ranal_demo` - demo of the ranal GUI library
- `swm` - simple window manager / compositor (sprot v0.1) (swm = simple window manager)
- `sprot_hello` - minimal sprot client (draws a colored rectangle)
- `term` - GUI terminal built on ranal (standalone or as a swm client via `--swm`)
framework parts:

- `libs/net` - network library for URL parsing, HTTP and transport
- `libs/TUI` - TUI library
- `libs/elf` - ELF parser lib
- `libs/memory` - checked allocation helpers
- `libs/srapi` - simple rendering API (srapi = simple rendering api)
- `libs/gui/ranal` - retained-mode GUI library on top of srapi (ranal = raylib analog (name deprecated))
- `libs/gui/sprot` - simple wire protocol for compositor/client comms (unix socket + memfd buffers) (sprot = simple protocol)
- `libs/gui/swm` - simple window manager / compositor (sprot v0.1) (swm = simple window manager)
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


## How version counting works
x.r.f
x = when r > 9
r = global update or new tool
f = small update or bug fix

## License

### This project contains multiple licensing schemes:

#### - mofl/ -  BSD 2-Clause
#### - other - MPL 2.0
