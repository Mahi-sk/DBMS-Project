# mini-dbms

A small disk-backed relational database engine written from scratch in C++17 —
no external DB libraries, no SQLite bindings. It implements the actual pieces
that make a database a database: page-based disk storage, a B+Tree index,
row serialization, and a hand-written SQL parser.

```
mini-dbms> CREATE TABLE users (id INT, name VARCHAR(32), email VARCHAR(64))
Table 'users' created (3 columns, primary key: id).
mini-dbms> INSERT INTO users VALUES (1, 'alice', 'alice@example.com')
1 row inserted.
mini-dbms> SELECT * FROM users WHERE id = 1
id | name | email
1 | alice | alice@example.com
(1 row)
```

## Why this exists

Most student projects call `sqlite3` or an ORM. This one *is* the layer
underneath that — built to understand (and be able to explain in an
interview) how indexing, disk I/O, and query execution actually work.

## Architecture

```
 SQL text
    |
    v
+-----------+     tokenize / parse       +-------------+
|  sql.cpp  |  ------------------------> |   table.cpp |
| (parser)  |                            | (schema,    |
+-----------+                            |  row (de)   |
                                          |  serialize) |
                                          +------+------+
                                                 |
                                                 v
                                          +-------------+
                                          |  btree.cpp  |
                                          | (B+Tree:    |
                                          |  search,    |
                                          |  insert,    |
                                          |  node split)|
                                          +------+------+
                                                 |
                                                 v
                                          +-------------+
                                          |  pager.cpp  |
                                          | (4KB pages, |
                                          |  disk I/O,  |
                                          |  page cache)|
                                          +-------------+
                                                 |
                                                 v
                                            table.db file
```

**Pager** (`pager.h/cpp`) — hides the filesystem behind `getPage(n)` /
`allocatePage()`. Pages are 4096 bytes, cached in memory on first read, and
flushed back to disk on close.

**B+Tree** (`btree.h/cpp`) — the index. Every table is one B+Tree keyed by
its (integer) primary key. Leaf nodes hold the actual row bytes and are
linked together (`next_leaf`) so a full table scan is just a walk down the
linked list — the same trick SQLite uses. Internal nodes hold routing keys
and child page pointers. Insertion handles the full split cascade: a full
leaf splits into two leaves, and if a parent is currently the root, a brand
new root is created above it (root page number is reused/rewritten in place
so nothing else has to update its "root pointer").

**Table** (`table.h/cpp`) — schema definition (`INT`, `VARCHAR(n)` columns),
binary row serialization (fixed-width rows so B+Tree cell sizes are static),
and the metadata page (page 0) that stores column definitions so a table can
be closed and reopened later.

**SQL layer** (`sql.h/cpp`) — a small hand-rolled parser (no parser
generator) for:
```sql
CREATE TABLE name (col TYPE, col TYPE, ...)     -- TYPE: INT | VARCHAR(n)
INSERT INTO name VALUES (v1, v2, ...)
SELECT * FROM name
SELECT * FROM name WHERE <conditions>
UPDATE name SET col = value [, col = value ...] [WHERE <conditions>]
DELETE FROM name [WHERE <conditions>]
ALTER TABLE name RENAME COLUMN old TO new
```
`<conditions>` is one or more `col OP value` comparisons (`OP` is `=`, `!=`,
`<`, `>`, `<=`, `>=`) joined by `AND` *or* `OR` — mixing both in one clause
isn't supported. A single `pk = value` condition hits the B+Tree directly
(`O(log n)`); anything else (other columns, ranges, multiple conditions)
falls back to a full table scan, since there are no secondary indexes.

## Building

```bash
mkdir build && cd build
cmake ..
make
./mini_dbms mydb.db
```

Or without CMake:
```bash
g++ -std=c++17 -O2 -o mini_dbms src/*.cpp
./mini_dbms mydb.db
```

The database file is a single binary file — delete it to start fresh.
Reopening an existing file with the same path automatically reloads its
schema, root page, and all rows.

## Design decisions worth knowing for an interview

- **Why B+Tree and not a plain B-Tree?** Leaf-to-leaf linked lists make
  full scans and range queries O(n) without re-walking the tree for every
  row, at the cost of some duplicated keys in internal nodes.
- **Why fixed-width rows?** It makes leaf-node capacity (`leafMaxCells`)
  computable up front from `PAGE_SIZE` and the schema, which keeps the
  split logic simple. The tradeoff: `VARCHAR(n)` always reserves `n` bytes
  even for short strings (real engines use variable-length records with a
  slotted-page layout to avoid this).
- **Root page number stays stable across splits.** When the root splits,
  its *contents* are copied into a new left-child page, and the *original*
  root page is overwritten in place to become the new internal root. That
  way the metadata page never has to be updated with a new root pointer.
- **Known limitation:** internal nodes don't yet recursively split when
  they fill up (only leaves do) — see the comment in `internalInsert()` in
  `btree.cpp`. Tested to ~490,000 rows for a typical row size before this
  limit is hit; it fails with a clear error rather than crashing or
  corrupting data. It's an intentional, documented scope cut rather than
  an oversight — a natural "what would you add next?" answer for an
  interview.
- **DELETE removes cells from their leaf but doesn't merge underfull
  leaves with siblings** (see `BTree::remove()` in `btree.cpp`). Lookups,
  inserts, and scans all remain correct afterward — it just means heavy
  delete workloads don't reclaim page space or keep the "half full" B+Tree
  invariant. Node merging/rebalancing on delete is the natural next step.
- **No secondary indexes, joins, or aggregate functions (COUNT/SUM/AVG).**
  Any `WHERE` not on a single `pk =` condition is a full table scan.
- **The primary key column is immutable** — `UPDATE ... SET <pk> = ...`
  is rejected, since changing it would mean moving the row to a different
  position in the tree rather than a simple in-place byte overwrite.

## Suggested resume line

> Built a disk-backed relational database engine in C++ from scratch,
> implementing page-based storage, a B+Tree index with node-splitting
> insertion, fixed-width row serialization, and a hand-written SQL parser
> supporting CREATE/INSERT/SELECT/UPDATE/DELETE/ALTER TABLE with
> multi-condition WHERE clauses (AND/OR, six comparison operators) and a
> natural-language front end (local ML classifier + LLM fallback);
> verified correctness across 20,000+ inserts including multi-level tree
> splits, deletes, and data persistence across process restarts.
