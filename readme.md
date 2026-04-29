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

1. Add a line to [`extra_tools.manifest`](./extra_tools.manifest)
2. Run `make`

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


## How version counting works
x.r.f
x = when r > 9
r = global update or new tool
f = small update or bug fix

## License

This project is distributed under MPL-2.0.
