# Natural-Language Interface for mini-dbms

Type plain English, get it translated into the SQL your C++ engine already
understands, and see it executed for real. This isn't a wrapper that fakes
results — every query actually runs through your B+Tree-backed storage
engine.

```
nl-dbms> create a table called users with id int, name varchar 32, email varchar 64
  -> [CREATE_TABLE 67% confident] CREATE TABLE users (id INT, name VARCHAR(32), email VARCHAR(64))
  Table 'users' created (3 columns, primary key: id).

nl-dbms> add a user with id 1 name alice and email alice@example.com to users
  -> [INSERT 55% confident] INSERT INTO users VALUES (1, 'alice', 'alice@example.com')
  1 row inserted.

nl-dbms> find the user where id equals 1
  -> [SELECT 66% confident] SELECT * FROM users WHERE id = 1
  id | name | email
  1 | alice | alice@example.com
  (1 row)
```

## How it works

```
 "show me all users"
        |
        v
 +------------------+     TF-IDF + Logistic       +------------------+
 |  train_model.py  | --------------------------> |  intent_model    |
 |  (offline, once) |     Regression classifier   |  .joblib         |
 +------------------+                             +--------+---------+
                                                            |
                                                            v
                                                  +--------------------+
                                                  |   nl_to_sql.py     |
                                                  | 1. classify intent |
                                                  |    (CREATE/INSERT/ |
                                                  |     SELECT)        |
                                                  | 2. extract slots   |
                                                  |    (table, cols,   |
                                                  |     values) via    |
                                                  |    regex           |
                                                  | 3. track schema so |
                                                  |    INSERT knows    |
                                                  |    column order    |
                                                  +---------+----------+
                                                            |
                                                            v
                                                     "SELECT * FROM
                                                      users WHERE id = 1"
                                                            |
                                                            v
                                                  +--------------------+
                                                  |     repl.py        |
                                                  | spawns the real    |
                                                  | mini_dbms.exe,     |
                                                  | feeds it the SQL,  |
                                                  | captures the       |
                                                  | actual result      |
                                                  +--------------------+
```

**Intent classification** is a genuinely trained ML model (TF-IDF vectorizer
+ Logistic Regression), not a keyword lookup table — it generalizes to
phrasings it wasn't trained on verbatim. It's intentionally small (~40
training examples) so it trains in well under a second; that's a deliberate
tradeoff for a project like this, not a limitation of the approach itself.

**Slot extraction** (pulling out the table name, column definitions, or
values) uses regex tailored to this specific SQL grammar. This mirrors how
real NLU systems like Rasa are structured: a trained intent model paired
with rule-based or learned slot-fillers, rather than one model doing
everything end to end.

**Schema tracking**: because the engine's `INSERT INTO name VALUES (...)`
is positional, the NL layer needs to remember each table's column order.
It saves that to a small `<dbfile>.schema.json` sidecar file next to your
database, so it survives you closing and reopening the REPL.

**Auto primary key**: the engine requires the first column of every table
to be `INT` (it's the B+Tree key). If your sentence doesn't include one,
the translator handles it automatically instead of erroring out:
- if you *did* specify some `INT` column, it's moved to the front
- if you specified no `INT` column at all, a surrogate `id INT` column is
  added, and its value is auto-generated on every `INSERT` (like a
  real auto-increment primary key) rather than asking you to supply one
  you never mentioned

Which tables got a synthetic id is also saved to the sidecar file, so this
keeps working correctly across sessions.

## Setup

From the `nlp/` folder:

```powershell
pip install -r requirements.txt
python train_model.py
```

This produces `intent_model.joblib`. You only need to redo this if you
change `dataset.py`.

## Optional: LLM fallback for phrasing the local model can't handle

The local classifier + regex slot-filler is fast and fully offline, but
it's a rule-based system at the slot-extraction layer — it will never
cover every possible phrasing. When it's not confident (or extraction
fails), `nl_to_sql.py` automatically falls back to calling an LLM instead,
**if and only if** the `ANTHROPIC_API_KEY` environment variable is set.
With no key set, behavior is unchanged — fully local, fully offline.

To enable it:

1. Get an API key from [console.anthropic.com](https://console.anthropic.com)
   (Settings -> API Keys).
2. Set it as an environment variable before running `repl.py`:

   **PowerShell (current session only):**
   ```powershell
   $env:ANTHROPIC_API_KEY = "sk-ant-..."
   python repl.py mydb.db
   ```

   **PowerShell (permanently, for your user account):**
   ```powershell
   [Environment]::SetEnvironmentVariable("ANTHROPIC_API_KEY", "sk-ant-...", "User")
   ```
   (restart your terminal afterward for it to take effect)

No new pip package needed — `llm_fallback.py` uses only Python's built-in
`urllib`. When it kicks in, the REPL shows `[via LLM fallback]` instead of
a confidence percentage, so you can always tell which path handled a given
query — useful for a demo, and for being honest about which layer did the
work if asked in an interview.

**Cost**: a small fraction of a cent per fallback call. It only fires when
the local model is uncertain, not on every query.

## Running it

Make sure the C++ engine is built first (from the project root):

```powershell
g++ -std=c++17 -O2 -o mini_dbms.exe src/main.cpp src/pager.cpp src/btree.cpp src/table.cpp src/sql.cpp
```

Then, from the `nlp/` folder:

```powershell
python repl.py mydb.db
```

`.exit` to quit. Without an API key, confidence below 45% will prompt you
to rephrase instead of guessing. With one set, it tries the LLM instead of
giving up.

## What it can and can't handle

Matches the engine's own grammar (see the main README): `CREATE TABLE`,
`INSERT`, and `SELECT` with either `*` or a primary-key `WHERE`. No
`UPDATE`/`DELETE`/joins/aggregates — same scope cuts as the base engine,
enforced in the LLM fallback's system prompt too, so it won't invent
syntax the engine can't run.

## Natural next steps (good "what would you add" answers)

- Replace the local intent classifier with a properly trained
  sequence-to-sequence / semantic-parsing model (e.g. fine-tuned on
  WikiSQL/Spider-style data) once you have real usage data — the current
  ~40-example dataset is intentionally small.
- Log every case where the LLM fallback fired, and periodically fold those
  examples back into `dataset.py` to retrain the local model, so the
  fast/free/offline path gradually covers more of what people actually type.
- Cache repeated LLM translations (e.g. by hashing the input text + known
  schema) to cut down on repeat API calls during a demo.
