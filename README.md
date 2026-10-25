# MySQL Worktrees

This is MySQL 9.7.4 with one addition: **`CREATE WORKTREE`**. It makes an instant, copy-on-write copy of a schema inside the same server. A worktree is an ordinary schema. You can read, write, run DDL on and drop it without affecting its source or any other worktree. It shares disk blocks with its source until pages diverge, so fifty copies of a 100 GB schema cost about 100 GB plus whatever each one writes.

The usual use is a **golden** schema that you load once and refresh from time to time, plus one throwaway worktree per developer, agent, branch or test run. All of them live in one `mysqld`, behind one port and one buffer pool.

Everything outside InnoDB's clone path is stock 9.7.4. Clients, drivers, `mysqldump` and replication protocols are unchanged.

Oracle's original MySQL notice is in [`README`](README).

## Install

Prebuilt binaries are attached to the [releases](https://github.com/matterialize/mysql-server/releases):

| Target | File | Notes |
|---|---|---|
| macOS 14+ on Apple Silicon | `mysql-9.7.4-wt*-macos-arm64.tar.gz` | OpenSSL is bundled |
| Linux arm64 (Graviton, Docker on Apple Silicon) | `mysql-9.7.4-wt*-linux-aarch64.tar.gz` | glibc 2.39+ (Ubuntu 24.04), needs `libssl3` |
| Linux x86_64 | `mysql-9.7.4-wt*-linux-x86_64.tar.gz` | glibc 2.39+ (Ubuntu 24.04), needs `libssl3` |

```bash
gh release download -R matterialize/mysql-server -p '*macos-arm64*'   # or curl the asset URL
mkdir -p ~/mysql-wt && tar -xzf mysql-9.7.4-wt*-macos-arm64.tar.gz -C ~/mysql-wt --strip-components=1
xattr -dr com.apple.quarantine ~/mysql-wt 2>/dev/null   # only needed if a browser downloaded it (unsigned binaries)
```

Set up a data directory and start the server:

```bash
cat > ~/mysql-wt/my.cnf <<'EOF'
[mysqld]
basedir = ~/mysql-wt
datadir = ~/mysql-wt/data
socket  = /tmp/mysql-wt.sock
port    = 3306
innodb_buffer_pool_size = 8G
# Required for overlay mode (see "Modes" below); harmless in reflink mode.
innodb_thread_concurrency = 64
# Many agents writing at once fill a small redo log quickly.
innodb_redo_log_capacity = 4G
# Each worktree table is an open file: size these for every live worktree
# (see "Open files" below). 20000 covers ~100 worktrees of a 180-table schema.
open_files_limit = 65536
innodb_open_files = 20000
table_open_cache = 20000
table_definition_cache = 20000
EOF
~/mysql-wt/bin/mysqld --defaults-file=~/mysql-wt/my.cnf --initialize-insecure
ulimit -n 65536   # the shell's limit caps open_files_limit; macOS defaults to 256
~/mysql-wt/bin/mysqld --defaults-file=~/mysql-wt/my.cnf &
~/mysql-wt/bin/mysql -uroot -S /tmp/mysql-wt.sock
```

(Write real paths in `my.cnf`. MySQL doesn't expand `~` there.) The macOS build has no X Plugin, so leave out `mysqlx` options or write them as `loose-mysqlx...`.

### Docker

The Linux arm64 and x86_64 tarballs run on `ubuntu:24.04`. A minimal image:

```dockerfile
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends libssl3 libaio1t64 && rm -rf /var/lib/apt/lists/*
ADD mysql-9.7.4-wt1-linux-aarch64.tar.gz /opt/
RUN mv /opt/mysql-9.7.4-wt1-linux-aarch64 /opt/mysql && useradd -r mysql && mkdir /var/lib/mysql && chown mysql /var/lib/mysql
USER mysql
CMD ["sh", "-c", "[ -d /var/lib/mysql/mysql ] || /opt/mysql/bin/mysqld --initialize-insecure --datadir=/var/lib/mysql; exec /opt/mysql/bin/mysqld --datadir=/var/lib/mysql --bind-address=0.0.0.0 --innodb-thread-concurrency=64"]
```

Put the data directory on a **named volume** (`-v mysqlwt:/var/lib/mysql`), not a bind mount from the Mac. Named volumes live on the Docker VM's ext4 disk, where overlay mode is tested. Bind mounts go through the VM's file-sharing layer, which hasn't been tested.

## Using worktrees

```sql
-- Load a golden schema once, the normal way (mysqldump, LOAD DATA, an ETL job, ...).
CREATE DATABASE golden;
-- ... load ...

-- Freeze it. A read-only source makes CREATE WORKTREE stall nothing, and golden's files stay untouched.
ALTER SCHEMA golden READ ONLY = 1;

-- One worktree per agent / branch / test run. Instant, regardless of size.
CREATE WORKTREE agent_1 FROM golden;
CREATE WORKTREE agent_2 FROM golden;

-- Use it like any schema: point the connection's database at it.
USE agent_1;
INSERT ...; UPDATE ...; DELETE ...; ALTER TABLE ...;

-- Stack: a worktree of a worktree starts from that worktree's current data.
CREATE WORKTREE agent_1_experiment FROM agent_1;

-- Throw it away.
DROP DATABASE agent_1_experiment;
```

Many agents can create worktrees at the same time. Creates from a read-only source run in parallel. Creates from a writable source take turns, since each one briefly blocks writes to the source.

**Refreshing golden.** Make it writable, apply your changes and make it read-only again:

```sql
ALTER SCHEMA golden READ ONLY = 0;
-- ... apply migrations / sync new rows ...
ALTER SCHEMA golden READ ONLY = 1;
```

Existing worktrees keep the data they were created with. New worktrees get the refreshed data. To move a worktree to the new data, drop it and create it again.

In reflink mode, `READ ONLY = 1` after a refresh also rewrites, contiguously, any of golden's table files that the refresh fragmented. A refresh writes golden in place while worktrees share its blocks, and on APFS every later `CREATE WORKTREE` would pay for each fragment. The rewrite takes about as long as copying those files. Until the worktrees created before the refresh are dropped, the rewritten files no longer share blocks with them, so they take that much extra disk. A file is skipped (with a note in the error log) when the disk lacks room for it.

If you'd rather build each refresh off to the side, make it a worktree. Run `CREATE WORKTREE golden_v2 FROM golden`, load into `golden_v2`, set it `READ ONLY = 1`, point new worktrees at it, then drop the old golden. A read-only schema can't be dropped, so run `ALTER SCHEMA golden READ ONLY = 0` before `DROP DATABASE golden`. Old worktrees keep working after their source is dropped.

**What a worktree copies:** every table (with its data, indexes, `AUTO_INCREMENT`, foreign keys, FULLTEXT indexes, encrypted and `ROW_FORMAT=COMPRESSED` tables, partitions), plus views, stored procedures and functions, triggers and events. Inside copied objects, references to `golden.x` are rewritten to the worktree when the worktree has `x`. Copied events start `DISABLED`. Grants are not copied, because they are per schema. Grant access to `agent_%` patterns if you want one grant to cover every worktree.

**Writable sources.** `CREATE WORKTREE` from a schema that isn't read-only works too. Writes to the *source* stall briefly while its dirty pages are flushed, which is about the time a `FLUSH TABLES ... FOR EXPORT` takes. Reads continue. A read-only golden avoids the stall completely.

**Replication.** `CREATE WORKTREE` is written to the binlog as one DDL statement. A replica running this build makes its own local worktree. A stock replica stops with a clear error.

## Modes

The server picks how files are cloned at startup (`innodb_worktree_mode`, read-only, default `AUTO`):

| Mode | When `AUTO` picks it | How it works |
|---|---|---|
| **REFLINK** | The data directory is on APFS, XFS with `reflink=1`, or btrfs | Each table file is a filesystem clone (`clonefile` / `FICLONE`). Worktree files are fully independent. |
| **OVERLAY** | Filesystems without file clones (ext4, including Docker's VM disk) | Each worktree table is a sparse file holding only the pages it has written. Every other page is read from the parent's immutable layer in `#wtlayers/`. Layers are reference-counted and removed when no worktree uses them, after `DROP` and at startup. Chains are capped at 16 levels and flattened automatically. |

`OVERLAY` can be forced on any filesystem (`--innodb-worktree-mode=OVERLAY`), which is how it is tested on XFS and APFS.

Overlay mode enforces settings that keep its crash recovery sound, and refuses `CREATE WORKTREE` with a clear message otherwise:
- `innodb_thread_concurrency > 0` (the default 0 is refused)
- the doublewrite buffer on
- a checksum algorithm other than `none`
- an fsyncing `innodb_flush_method`
- redo logging enabled
- a page size of 16 KB or less

`ALTER WORKTREE agent_1 MATERIALIZE` copies every page a worktree still inherits into its own files. That makes it a standalone stock-format schema that no longer depends on layers. It runs online, under writes.

**Recommended filesystem:** XFS with `reflink=1` on Linux (AWS: `mkfs.xfs -m reflink=1`, the default in recent xfsprogs), and APFS on macOS. Use ext4 only where you can't choose, for example inside Docker.

## Things to know

- **Disk usage:** `du` and `ls` show each worktree at full size, because shared blocks are counted once per file. Use `df` to see real usage.
- **Memory:** worktrees don't share buffer-pool pages with their source or with each other. A hot page read in 50 worktrees is cached 50 times. Size `innodb_buffer_pool_size` for the combined working set.
- **Open files:** every worktree table is a tablespace file (one per table, plus one per partition and about a dozen per FULLTEXT index). A trial with a 180-table schema held about 180 open files per worktree. Set `innodb_open_files`, `table_open_cache` and `table_definition_cache` to at least 1.2 × (files per worktree × live worktrees, golden included), and `open_files_limit` above that. Raise the shell's limit (`ulimit -n`) before starting `mysqld`, because it caps `open_files_limit`.
- **Redo log:** with dozens of agents writing at once, a 1 GB redo log fills fast and writers wait for checkpoints (`Threads are unable to reserve space in redo log` in the error log). Use `innodb_redo_log_capacity = 4G` or more.
- **Not supported (refused before anything changes):** non-InnoDB tables, tables in shared or general tablespaces (only `innodb_file_per_table` tables), `DATA DIRECTORY` tables, `COMPRESSION=` (page compression), schemas with JavaScript libraries, system schemas, and a server in `--read-only` mode. On overlay tables, `DISCARD/IMPORT TABLESPACE`, `FLUSH TABLES ... FOR EXPORT` and the clone plugin are refused.
- **Schema names in your SQL:** only references inside copied views, routines, triggers and events are rewritten. If application SQL hard-codes `golden.table`, it still reads golden. Use unqualified names, or `DATABASE()`.
- Windows isn't supported.

## How it works, and how it was tested

The InnoDB change is "lazy import". A worktree's table files are clones of the source's, so on disk their pages still carry the source's tablespace and index ids. Pages are translated as they're read into the buffer pool, and only pages that are really modified get written back with the worktree's identity. Page 0 of each file records its ancestors. Crash recovery, LOBs, change buffering, compressed and encrypted pages, and redo for overlay pages all go through the same translation.

Tests are in `mysql-test/suite/worktree` (29 tests, each run in reflink and overlay combinations):

```bash
mysql-test/mysql-test-run.pl --suite=worktree
```

Testing behind this release:
- the worktree suite on APFS, XFS, btrfs and ext4;
- kill -9 loops of 50 iterations with a replica, compressed and encrypted tables, and kills inside `CREATE WORKTREE`, freeze and flatten, all passing 50/50;
- 10 GB, 75-table schemas cloned in 0.02–0.34 s at about 0.03% of the source's disk;
- the stock `innodb`, `innodb_zip`, `encryption` and `rpl` suites, with no new failures against stock 9.7.4.

## Building from source

Like stock MySQL, with no extra dependencies:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWITH_ROUTER=OFF -DWITH_SSL=system   # macOS: -DWITH_SSL=$(brew --prefix openssl@3)
ninja -C build
```

The change set is the `worktrees` branch on top of the `mysql-9.7.4` tag. It's licensed under GPLv2, like MySQL.
