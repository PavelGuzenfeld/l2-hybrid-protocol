# Static Binary Build Guide

## The Problem

`musl-gcc` on Ubuntu/Debian is a **C-only wrapper**. It doesn't include C++ standard library headers (`<algorithm>`, `<vector>`, etc.), so it can't compile C++ code.

## Solutions

### Option 1: Docker + Alpine (Recommended)

The cleanest way to build musl C++ binaries. Alpine Linux uses musl natively.

```bash
# one command - builds everything
./scripts/build_musl.sh

# output: build/musl-alpine/bin/
```

Or manually:
```bash
docker build -f Dockerfile.musl -t l2net-musl-builder .
docker run --rm -v $(pwd)/build:/src/build l2net-musl-builder
```

### Option 2: Static glibc build

Less portable than musl, but works on most Linux systems:

```bash
cmake -B build/static \
    -DL2NET_STATIC=ON \
    -DL2NET_BUILD_BENCHMARKS=OFF \
    -DL2NET_BUILD_TESTS=OFF \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build/static
```

**Requires**: `sudo apt install libc6-dev` (static glibc)

### Option 3: Native musl system (Alpine/Void Linux)

If you're already on a musl-based system:

```bash
cmake -B build/release \
    -DL2NET_USE_MUSL=ON \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build/release
```

## Files to add to your project

```
cmake/MuslSupport.cmake    - handles both musl and glibc static builds
Dockerfile.musl            - Alpine-based build container
scripts/build_musl.sh      - convenience script for Docker builds
```

## CMakeLists.txt changes

Add after `project()`:
```cmake
option(L2NET_USE_MUSL "Build with musl (requires musl system)" OFF)
option(L2NET_STATIC "Build fully static binaries with glibc" OFF)
include(cmake/MuslSupport.cmake)
```

Modify static linking section:
```cmake
if(NOT L2NET_USE_MUSL AND NOT L2NET_STATIC)
    # normal glibc builds: just static-link libstdc++
    add_link_options(-static-libstdc++ -static-libgcc)
endif()
```

## Verify static binary

```bash
file build/musl-alpine/bin/l2net_remote_node
# should say: "statically linked"

ldd build/musl-alpine/bin/l2net_remote_node
# should say: "not a dynamic executable"
```

## Why musl?

| Feature | glibc dynamic | glibc static | musl static |
|---------|--------------|--------------|-------------|
| Portable | ❌ version-locked | ⚠️ large, some issues | ✅ works everywhere |
| Size | small | huge (~10MB+) | medium (~2-5MB) |
| Alpine compatible | ❌ | ❌ | ✅ |
| Debian/Ubuntu | ✅ | ⚠️ requires static libs | ✅ via Docker |