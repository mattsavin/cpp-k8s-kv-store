# cpp-k8s-kv-store

A learning project built to explore key-value store internals — log-structured storage, crash recovery, distributed replication, and Kubernetes deployment — in the context of preparing for roles working on systems like IBM Ceph.

The implementation is intentionally simple and hands-on: a C++ append-only log with an HTTP API, deployed as a StatefulSet with a persistent volume. The design document describes both what is currently implemented and what a production-grade system (like Ceph's BlueStore) does differently.

The code is verbosely commented — particularly in `kv_engine.h` — because this was a first encounter with several C++ features (`fstream`, `shared_mutex`,  etc.). The comments narrate what each line does as a learning aid, not as production documentation practice.

## Contents

- `main.cpp` — application entrypoint / simple server
- `kv_engine.h` — core key-value engine interfaces
- `httplib.h`, `json.hpp` — single-file dependencies bundled for convenience
- `DESIGN.md` — WAL format and recovery design (detailed)

## Requirements

- C++17 or later
- CMake (3.10+ recommended)
- A POSIX-compatible OS (Linux/macOS) for `fsync` semantics

## Build

From the repo root:

```bash
mkdir -p build
cd build
cmake ..
make -j$(nproc)
```

When the build completes the binary will be placed in the build directory (e.g. `build/kv_store` on Linux/macOS).

## Run

Run the binary from the `build/` directory:

```bash
./kv_store
```

## Design & WAL

See [DESIGN.md](DESIGN.md) for the on-disk record format, recovery algorithm, and planned improvements (checksums, segment rotation, checkpointing, and Ceph-aligned multi-replica replication via Redis Streams).
