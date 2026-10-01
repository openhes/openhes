In the current Dockerfile, do I really have to use this way to install packages for arm64? Can't I just use apt install to install these packages?

The short answer is: **not reliably**. The current approach is a well-known pattern for cross-compilation Dockerfiles. Here is why:

## Why `apt-get install libc6-dev:arm64` is problematic

Even though you've added `dpkg --add-architecture arm64` and configured the repos, running `apt-get install` for `:arm64` packages on an **amd64 host** will:

1. Install the arm64 `.deb` files
2. **Run post-installation scripts** (`postinst`) - these scripts are **arm64 ELF binaries** that cannot run on an amd64 kernel without QEMU `binfmt_misc`
3. This causes `dpkg` to fail mid-install, leaving the package database in a broken state

For example, `libc6:arm64`'s `postinst` calls `ldconfig` with arm64 libraries, and `libglib2.0-dev:arm64` may try to run `glib-compile-schemas`. Both will fail.

## The current approach is the proven solution

`apt-get download` + `dpkg-deb -x` avoids running any postinst scripts at all. It is just a **tarball extraction**. You get all the headers and `.so` files you need, which is exactly what cross-compilation requires. This is the same strategy used by:

- **Yocto/OpenEmbedded** (populate_sysroot)
- **crosstool-NG**
- Many production cross-compilation Docker images

## But there *is* a slightly cleaner alternative

If you want something more maintainable, you could wrap it in a helper function to reduce duplication:

```dockerfile
# Helper: download an arm64 .deb and extract to sysroot
RUN mkdir -p /tmp/sysroot && cd /tmp/sysroot && \
    for pkg in \
        libc6-dev:arm64 libstdc++-14-dev:arm64 linux-libc-dev:arm64 \
        libglib2.0-dev:arm64 libhiredis-dev:arm64 libxml2-dev:arm64 \
        libbluetooth-dev:arm64 zlib1g-dev:arm64 libssl-dev:arm64 \
        uuid-dev:arm64 libelf-dev:arm64 libpcre2-dev:arm64 \
        libbz2-dev:arm64 liblzma-dev:arm64 libmount-dev:arm64 \
        libselinux1-dev:arm64; \
    do \
        apt-get download "$pkg" 2>/dev/null; \
    done && \
    for f in *.deb; do dpkg-deb -x "$f" /tmp/sysroot-root/ 2>/dev/null || true; done
```

Or shorter: use a single `for` loop with an inline extraction:

```dockerfile
RUN mkdir -p /tmp/sysroot && cd /tmp/sysroot && \
    apt-get download \
        libc6-dev:arm64 libstdc++-14-dev:arm64 linux-libc-dev:arm64 \
        libglib2.0-dev:arm64 libhiredis-dev:arm64 libxml2-dev:arm64 \
        libbluetooth-dev:arm64 zlib1g-dev:arm64 libssl-dev:arm64 \
        uuid-dev:arm64 libelf-dev:arm64 libpcre2-dev:arm64 \
        libbz2-dev:arm64 liblzma-dev:arm64 libmount-dev:arm64 \
        libselinux1-dev:arm64 2>/dev/null && \
    for f in *.deb; do dpkg-deb -x "$f" /tmp/sysroot-root/ 2>/dev/null || true; done
```

**Summary:** the current approach is correct and battle-tested for cross-compilation. You *could* make `apt-get install` work with `DPkg::Options::=--force-all` and `QEMU_LD_PREFIX` tricks, but it is far more brittle. Do not change it.

---

In #file:Dockerfile.crossbuild:78-92 , I see that you copy files from sysroot to /usr/aarch64-linux-gnu/**. Is this the best practice? Is putting files under /usr/<arch> the common practice?

Yes, **`/usr/aarch64-linux-gnu` is the standard convention** for cross-compilation sysroots on Linux. Here is why:

## This is exactly what the toolchain expects

When you install `gcc-aarch64-linux-gnu` (via apt), the compiler is **pre-configured** to search for libraries and headers in specific paths. You can verify this:

```bash
aarch64-linux-gnu-gcc -print-search-dirs
aarch64-linux-gnu-gcc -print-multiarch      # -> aarch64-linux-gnu
```

The multiarch triplet `aarch64-linux-gnu` is baked into the compiler's search path. So placing libraries in `/usr/aarch64-linux-gnu/lib/` and headers in `/usr/aarch64-linux-gnu/include/` means the cross-compiler will **find them automatically** without any extra `-I` or `-L` flags.

## This mirrors the Debian/Ubuntu multiarch convention

On a real arm64 system (or an arm64 chroot), packages install to:

- `/usr/lib/aarch64-linux-gnu/` - architecture-specific shared libraries
- `/usr/include` - headers (same path, but the compiler knows its triple)
- `/usr/lib/aarch64-linux-gnu/pkgconfig/` - `.pc` files

So the Dockerfile is essentially **recreating the sysroot layout** that a native arm64 system would have, just under `/usr/aarch64-linux-gnu/`, which is exactly where the cross-compiler looks.

## Other common patterns

There are a few variations in practice, all valid:

| Approach                                   | Description                                                                            |
|--------------------------------------------|----------------------------------------------------------------------------------------|
| **`/usr/aarch64-linux-gnu`** (yours)       | Simplified - headers + libs together under the triplet dir. Clean and works great.     |
| Full sysroot (e.g. `/sysroot`)             | A complete arm64 filesystem root (usr, lib, etc...). Used by Yocto/OpenEmbedded. Passed via `--sysroot` to gcc. |
| **`/usr/lib/aarch64-linux-gnu`** + include | Closest to actual Debian multiarch layout. Requires `-I/usr/include` to work. |

Your approach is actually **cleaner than a full sysroot** for this use case: you do not need a whole arm64 rootfs, only the development headers and libraries. And by putting them under `/usr/aarch64-linux-gnu/`, you avoid cluttering include with arm64 headers which could conflict with native amd64 development packages.

## tl;dr

**Your current approach is best practice.** It's simple, matches the cross-compiler's default search paths, avoids conflicts with native packages, and is widely used in production cross-compilation Docker images (e.g. Rust embedded, ARM GCC toolchains, etc.). No change needed.

---

Why #file:Dockerfile.crossbuild:94-103 is necessary?

Good question. This step installs the **native amd64** version of `libglib2.0-dev` (no `:arm64` suffix), and it serves two purposes:

## 1. Native build tools (`glib-genmarshal` et al.)

The `libglib2.0-dev` package ships host executables like:

| Tool | Purpose |
|---|---|
| `glib-genmarshal` | Generates GObject signal marshaller code |
| `glib-mkenums` | Generates enum type registration code |
| `glib-compile-resources` | Compiles `.gresource.xml` into binary resources |
| `glib-compile-schemas` | Compiles GSettings schema files |

These are **arm64 ELF binaries** if you install `libglib2.0-dev:arm64`, which means they **can't run** on the amd64 build host (without QEMU emulation). The native amd64 versions are needed so these tools execute correctly during the build process.

## 2. pkg-config metadata for the host

`pkg-config` itself is already installed (step 1), but it also needs `glib-2.0.pc` and related `.pc` files available for its own internal operations (some CMake or autotools setups query them from the host side during configuration).

## But wait: will the amd64 `.pc` files conflict with the arm64 ones?

No. They live in different paths:

- **amd64**: pkgconfig
- **arm64**: `/usr/aarch64-linux-gnu/lib/pkgconfig/`

And the toolchain file overrides `PKG_CONFIG_LIBDIR` to point exclusively to the arm64 path, so CMake's `pkg-config` calls will find the correct arm64 `.pc` files and ignore the amd64 ones.

## Summary

You can skip this step if none of your code uses GObject signals, GResources, or GSchema files, but GLib-based projects often do. The native tools are needed for **cross-compilation**, because you cannot run arm64 executables on amd64.

---

