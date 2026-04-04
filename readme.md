# OneTool

OneTool - a single static Linux binary that contains multiple built-in tools

The long-term idea is simple:
one executable, one CLI entrypoint, many capabilities implemented from scratch inside the project.



## status

implemented tools:

- `lastmod` - prints the last modification time of a file

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




## License

This project is distributed under MPL-2.0.
