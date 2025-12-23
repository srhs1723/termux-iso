# termux-iso

Small ISO9660 builder for Termux.

## Build

```sh
make
```

## Usage

```sh
./termux-iso -o output.iso /path/to/root
```

Optional boot image:

```sh
./termux-iso -o output.iso -b boot.img /path/to/root
```

The boot image is stored as `BOOTIMG.BIN` with a generated `BOOT.CAT` catalog.

### Advanced options

```sh
./termux-iso -o output.iso -V MY_VOLUME -S MY_SYSTEM -A MY_APP -P MY_PUBLISHER \
  -p MY_PREPARER -R MY_SET -x '*.tmp' -x cache/* -t 1700000000 /path/to/root
```

* `-V` sets the volume identifier.
* `-S` sets the system identifier.
* `-A` sets the application identifier.
* `-P` sets the publisher identifier.
* `-p` sets the data preparer identifier.
* `-R` sets the volume set identifier.
* `-x` excludes paths using `fnmatch` patterns (repeatable).
* `-t` forces a fixed UNIX timestamp for deterministic builds.

File names are normalized to ISO9660 level 1 (8.3) and collisions are resolved
by adding `~N` suffixes.
