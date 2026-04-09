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


## How version counting works
x.r.f
x = when r > 9
r = global update or new tool
f = small update or bug fix

## License

This project is distributed under MPL-2.0.
