# Design: Log-Structured KV Store — Format and Recovery

This document describes the on-disk format, write semantics, and recovery algorithm as currently implemented in `kv_engine.h`, followed by planned improvements. The project is a learning exercise in KV store internals — log-structured storage, crash recovery, and distributed replication — with the planned improvements intentionally modelled on IBM Ceph's architecture.

---

## Current Implementation

### Terminology

- **Log file**: The single append-only file at `/data/wal.log` where all mutations are recorded.
- **OpCode**: A 1-byte tag identifying the record type (PUT or DELETE).
- **Index**: The in-memory `std::unordered_map<string, RecordLocation>` that maps keys to their on-disk location.
- **RecordLocation**: An `{offset, size}` pair pointing into the log file for a key's current value.

### Record Format (on-disk)

Records are written sequentially into a single binary log file. All integers use the platform-native width of `size_t` (8 bytes on 64-bit systems).

```text
[OpCode    : 1 byte  ] — 0x01 = PUT, 0x02 = DELETE
[Key size  : 8 bytes ] — length of key in bytes (size_t)
[Value size: 8 bytes ] — length of value in bytes (size_t); 0 for DELETE records
[Key data  : variable] — raw key bytes
[Value data: variable] — raw value bytes (absent for DELETE)
```

There are no magic bytes, version fields, LSNs, or checksums in the current format.

### Write path

- Every PUT or DELETE appends one record to the log file.
- After writing, `flush()` is called to push data to the kernel buffer. There are no `fsync()` calls; durability depends on the OS and the persistence of the underlying volume (a Kubernetes PVC in the reference deployment).
- The in-memory index is updated immediately after the write.
- Writes are serialized with a `std::unique_lock<std::shared_mutex>`.

### Read path

- GET operations take a `std::shared_lock` (allowing concurrent reads), look up the key in the index to get a `RecordLocation`, then seek to that offset in the log file and read `value_size` bytes.

### Recovery algorithm

On startup, `recover()` replays the entire log file to rebuild the in-memory index:

```text
offset = 0
loop:
  read OpCode (1 byte)
  read key_size (8 bytes)
  read value_size (8 bytes)
  read key (key_size bytes)
  if OpCode == PUT:
    record offset and size of the value region in the index
    seek past value_size bytes
  if OpCode == DELETE:
    remove key from index
  on any read error (EOF, short read): stop
```

The loop stops silently at the first read error, which handles the normal EOF case. Partially written records at the tail — which can occur if a crash interrupted a write — are dropped without detection: there are no checksums or length guards to distinguish a partial record from a clean EOF.

Recovery time scales linearly with log file size, since the entire log is always replayed from the beginning.

### Thread safety

| Operation      | Lock type                          |
|----------------|------------------------------------|
| `put`          | `unique_lock<shared_mutex>`        |
| `remove`       | `unique_lock<shared_mutex>`        |
| `get`          | `shared_lock<shared_mutex>`        |
| `get_all_data` | `shared_lock<shared_mutex>`        |

### Example: PUT record bytes

```
01                        # OpCode = PUT
08 00 00 00 00 00 00 00   # key_size = 8
05 00 00 00 00 00 00 00   # value_size = 5
6d 79 5f 6b 65 79 5f 31  # key = "my_key_1"
68 65 6c 6c 6f            # value = "hello"
```

### Known limitations

- **No corruption detection**: A torn write is silently dropped only if the partial record happens to fall at EOF. A corrupt byte mid-record will cause misparse and an unpredictable index.
- **No durability guarantee**: `flush()` without `fsync()` means data in the OS page cache can be lost on a host crash.
- **Unbounded recovery time**: The full log is replayed on every startup; as the log grows, so does startup latency.
- **No space reclamation**: Overwritten or deleted keys accumulate as dead records; the file only grows.

---

## Planned Improvements

### Record framing and checksums

Add a header with a magic cookie, payload length, and a CRC32/CRC64 trailer so that:

- Partial writes at the tail are detected by length mismatch or checksum failure and truncated cleanly.
- Corruption mid-file is detected before the record is applied.

Proposed layout:

```
[Magic     : 4 bytes ] — 0x4B56574C ("KVWL")
[Version   : 1 byte  ] — format version
[OpCode    : 1 byte  ] — PUT / DELETE
[Key size  : 2 bytes ] — max 65535-byte keys
[Value size: 4 bytes ] — max 4 GiB values
[Key data  : variable]
[Value data: variable]
[CRC32     : 4 bytes ] — checksum over all preceding bytes
```

### LSN and checkpointing

Assign a monotonically increasing Log Sequence Number to each record. Periodically flush the in-memory index to a snapshot file and write a CHECKPOINT record containing the snapshot's LSN. Recovery then only needs to replay records after the checkpoint LSN, bounding startup time regardless of total log history.

### Segment rotation and GC

Split the log into fixed-size segment files (e.g., 64 MiB each). After a checkpoint, segments fully covered by the checkpoint LSN can be deleted.

### Durability policy

Add a configurable `fsync` policy:

- `sync`: `fsync` on every write — safest, highest latency.
- `async`: rely on OS page cache — higher throughput, small loss window on host crash.

### Multi-replica support via Redis Streams (Ceph-aligned model)

The current deployment is a single-replica StatefulSet — scaling it to multiple pods is unsafe because each pod maintains its own independent in-memory index and log file, so replicas would diverge immediately.

The proposed replication design mirrors how Ceph's RADOS layer handles multi-OSD writes: a **primary-copy replication model** where one primary pod is the single write-ordering point, and replicas confirm writes before the client is acknowledged. Redis Streams serve the role of Ceph's **PG Log** — an ordered, durable, replayable replication log.

#### Mapping to Ceph concepts

| This project | Ceph equivalent |
| --- | --- |
| Primary pod | Primary OSD |
| Replica pod | Replica OSD |
| Redis Stream (`kv:pg-log`) | PG Log |
| LSN | `eversion_t` (epoch + version) |
| `min_replicas` config | `min_size` pool setting |
| Per-pod WAL (`wal.log`) | BlueStore/RocksDB WAL |
| Kubernetes StatefulSet | OSD acting set |

#### Replication write path

All client writes are routed to the primary pod (enforced via a Kubernetes headless Service targeting the pod with ordinal 0, or a Redis-based leader lock).

1. Primary appends the mutation to its local WAL and updates its in-memory index.
2. Primary appends the mutation to a Redis Stream (`XADD kv:pg-log * opcode PUT key foo value bar lsn 42`), receiving a stream entry ID.
3. Replica pods, each running a background reader loop (`XREAD BLOCK kv:pg-log`), receive the entry, apply it to their own WAL and index, then acknowledge back to the primary (via a per-replica Redis key, e.g., `kv:ack:<pod-id>`).
4. Primary waits until `min_replicas` acknowledgments are recorded (analogous to Ceph's `min_size`). Only then does it return success to the client.

This ensures that a write confirmed to the client is durable on at least `min_replicas` pods, matching Ceph's strong-consistency write guarantee.

#### Recovery (log-based, analogous to Ceph PG Log replay)

When a replica restarts it does not need a full data copy from the primary if the Redis Stream still contains the missing entries (Ceph calls this the **log-based recovery** fast path):

1. Replica reads its last applied stream entry ID from a local state file.
2. Replica calls `XREAD COUNT 10000 STREAMS kv:pg-log <last-id>` and replays each entry against its WAL and index.
3. Once caught up, the replica resumes its normal blocking-read loop.

If the replica has been down long enough that the required stream entries have been trimmed (controlled by `MAXLEN` on the stream, analogous to Ceph's PG Log length limit), a full resync is required — the primary exposes a `/snapshot` HTTP endpoint that streams a consistent copy of the current key-value state, after which the replica continues from the snapshot's LSN.

#### Placement

At small replica counts (2–3 pods) a static StatefulSet is sufficient. For larger clusters a CRUSH-style deterministic placement algorithm — mapping a hash of the key to a primary pod without a central lookup table — avoids coordination overhead on reads and allows clients to locate the correct primary independently.

#### Operational considerations

- Redis Streams retain history (`MAXLEN ~`) unlike plain pub/sub, enabling log-replay recovery without full resync for short outages.
- The Redis Stream itself must be durable (`appendfsync always` or AOF + RDB), otherwise it can lose entries and break replica recovery — the same reason Ceph's PG Log is flushed to disk before returning write acknowledgments.
- Leader election (primary designation) can use a Redis lock (`SET kv:primary <pod-id> NX EX 10`) with TTL-based renewal, similar to how Ceph Monitors maintain OSDMap authority via Paxos quorum.
