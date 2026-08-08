"""
Natural-language front end for mini-dbms.

Type plain English -> gets classified + slot-filled into SQL -> executed
against the actual compiled C++ engine -> result printed back.

Usage:
    python repl.py [dbfile]

Requires:
    - intent_model.joblib present (run `python train_model.py` once first)
    - the mini_dbms engine already built (mini_dbms.exe on Windows,
      mini_dbms on Linux/macOS) sitting in the project root, one level up
      from this file.
"""

import json
import os
import re
import subprocess
import sys

try:
    from dotenv import load_dotenv
    load_dotenv()  # reads nlp/.env (if present) into os.environ, e.g. ANTHROPIC_API_KEY
except ImportError:
    pass  # dotenv not installed -- fall back to whatever's already in the environment

from nl_to_sql import NLToSQL, TranslationError, AUTO_ID_PLACEHOLDER

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_engine_binary():
    candidates = ["mini_dbms.exe", "mini_dbms"] if os.name == "nt" else ["mini_dbms", "mini_dbms.exe"]
    for name in candidates:
        path = os.path.join(PROJECT_ROOT, name)
        if os.path.isfile(path):
            return path
    return None


def run_engine_command(binary_path, db_file, sql):
    """
    Spawns a fresh instance of the engine, feeds it exactly one SQL
    statement followed by `.exit`, and returns just that statement's
    output. State persists correctly across invocations because the
    engine flushes/reloads mydb's schema + rows to/from disk on
    close/open -- so this is equivalent to typing it into a long-running
    session, just simpler and more robust to drive from Python.
    """
    stdin_script = f"{sql}\n.exit\n"
    try:
        proc = subprocess.run(
            [binary_path, db_file],
            input=stdin_script,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except subprocess.TimeoutExpired:
        return "Error: engine process timed out."

    parts = proc.stdout.split("mini-dbms> ")
    if len(parts) < 2:
        return proc.stdout.strip() or (proc.stderr.strip() if proc.stderr else "(no output)")
    return parts[1].strip()


def resolve_auto_id(binary_path, db_file, sql):
    """
    If the translator left an AUTO_ID_PLACEHOLDER in an INSERT statement
    (because the table's primary key was auto-generated and the user never
    typed a value for it), figure out the next id by counting existing rows
    and substitute it in. Simple sequential-id scheme: fine for a table
    that's only ever populated through this auto-id path, since ids are
    never reused (no DELETE/UPDATE exist in this engine).
    """
    if AUTO_ID_PLACEHOLDER not in sql:
        return sql
    m = re.search(r"INSERT INTO (\w+)", sql, re.IGNORECASE)
    if not m:
        return sql
    table = m.group(1)
    count_result = run_engine_command(binary_path, db_file, f"SELECT * FROM {table}")
    rows_match = re.search(r"\((\d+)\s+rows?\)", count_result)
    next_id = int(rows_match.group(1)) + 1 if rows_match else 1
    return sql.replace(AUTO_ID_PLACEHOLDER, str(next_id))


def schema_sidecar_path(db_file):
    return db_file + ".schema.json"


def load_schema_sidecar(nlp, db_file):
    path = schema_sidecar_path(db_file)
    if not os.path.exists(path):
        return
    with open(path, "r") as f:
        data = json.load(f)
    if "schemas" in data:
        nlp.schemas = {table: [tuple(col) for col in cols] for table, cols in data["schemas"].items()}
        nlp.auto_id_tables = set(data.get("auto_id_tables", []))
    else:
        # old sidecar format from before auto_id_tables existed: the whole
        # file was just the schemas dict
        nlp.schemas = {table: [tuple(col) for col in cols] for table, cols in data.items()}


def save_schema_sidecar(nlp, db_file):
    path = schema_sidecar_path(db_file)
    with open(path, "w") as f:
        json.dump({
            "schemas": nlp.schemas,
            "auto_id_tables": sorted(nlp.auto_id_tables),
        }, f)


def main():
    db_file = sys.argv[1] if len(sys.argv) > 1 else "mydb.db"

    binary_path = find_engine_binary()
    if not binary_path:
        print("Couldn't find the mini_dbms engine binary in the project root.")
        print("Build it first, e.g.:")
        print("  g++ -std=c++17 -O2 -o mini_dbms.exe src/main.cpp src/pager.cpp "
              "src/btree.cpp src/table.cpp src/sql.cpp")
        sys.exit(1)

    try:
        nlp = NLToSQL()
    except FileNotFoundError as e:
        print(str(e))
        sys.exit(1)

    load_schema_sidecar(nlp, db_file)

    print("mini-dbms :: natural language mode")
    print(f"engine: {binary_path}")
    print(f"database: {db_file}")
    print("Type plain English. Examples:")
    print("  create a table called users with id int, name varchar 32, email varchar 64")
    print("  add a user with id 1 name alice and email alice@example.com to users")
    print("  show me all users")
    print("  find the user where id equals 1")
    print("Type .exit to quit.\n")

    while True:
        try:
            text = input("nl-dbms> ").strip()
        except EOFError:
            break
        if not text:
            continue
        if text in (".exit", ".quit"):
            break

        try:
            sql, intent, confidence, note = nlp.translate(text)
        except TranslationError as e:
            print(f"  ! {e}")
            continue

        sql = resolve_auto_id(binary_path, db_file, sql)

        if confidence is None:
            print(f"  -> [via LLM fallback] {sql}")
        else:
            print(f"  -> [{intent}, {confidence:.0%} confident] {sql}")
        if note:
            print(f"  (note: {note})")

        result = run_engine_command(binary_path, db_file, sql)
        print(f"  {result}\n")

        if sql.strip().upper().startswith("CREATE TABLE") and "Error" not in result:
            save_schema_sidecar(nlp, db_file)

    print("bye.")


if __name__ == "__main__":
    main()
