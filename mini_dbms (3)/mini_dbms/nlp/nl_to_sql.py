"""
Translates a natural-language sentence into a SQL statement that
mini-dbms's hand-written parser understands:

    CREATE TABLE name (col TYPE, col TYPE, ...)
    DROP TABLE name
    SHOW TABLES
    INSERT INTO name VALUES (v1, v2, ...)
    SELECT * FROM name
    SELECT * FROM name WHERE <conditions>
    UPDATE name SET col = value [, ...] [WHERE <conditions>]
    DELETE FROM name [WHERE <conditions>]
    ALTER TABLE name RENAME COLUMN old TO new

Architecture (a small, honest version of a real NLU pipeline):
    1. Intent classification -- trained ML model (TF-IDF + LogisticRegression)
       picks one of 8 intents: CREATE_TABLE, DROP_TABLE, SHOW_TABLES, INSERT,
       SELECT, UPDATE, DELETE, RENAME_COLUMN.
    2. Slot extraction -- regex-based, tailored to this narrow SQL grammar.
       (Real systems often mix a trained intent model with rule-based or
       learned slot-fillers too -- e.g. Rasa's NLU pipeline works this way.)
    3. Schema tracking -- INSERT/UPDATE/DELETE/RENAME_COLUMN all need to know
       column names/order/types, so we remember each table's columns from
       its CREATE_TABLE statement, update them on RENAME_COLUMN, and forget
       them on DROP_TABLE.
"""

import os
import re
import joblib

import llm_fallback

MODEL_PATH = os.path.join(os.path.dirname(__file__), "intent_model.joblib")

CONFIDENCE_THRESHOLD = 0.28  # below this, try the LLM fallback (if configured)
# Note: this was 0.45 back when there were only 4 intents (CREATE_TABLE,
# INSERT, SELECT, DROP_TABLE). With 8 intents now, correct top-1 picks
# naturally score lower since probability mass spreads across more
# classes -- what actually matters is the margin over the runner-up, which
# stays wide (commonly 30-40+ points) even when the top score itself is
# only ~30-50%. Re-tune this if the intent set grows again.

# placeholder used in generated SQL when a value for an auto-generated
# surrogate primary key needs to be filled in by the caller (repl.py),
# since NLToSQL itself doesn't talk to the running engine to know the
# next available id.
AUTO_ID_PLACEHOLDER = "__AUTO_ID__"


class TranslationError(Exception):
    pass


class NLToSQL:
    def __init__(self):
        if not os.path.exists(MODEL_PATH):
            raise FileNotFoundError(
                "intent_model.joblib not found. Run `python train_model.py` first."
            )
        self.model = joblib.load(MODEL_PATH)
        # table_name -> [(col_name, col_type), ...] in declared order
        self.schemas = {}
        # tables where the primary key was auto-generated (not typed by the
        # user), so INSERT should fill it in automatically rather than
        # asking for a value that was never part of the user's mental model
        self.auto_id_tables = set()
        # set by builders when they made a helpful automatic adjustment
        # (e.g. adding a surrogate INT primary key) worth telling the user about
        self.last_note = None

    # ---------- intent classification ----------

    def classify(self, text):
        proba = self.model.predict_proba([text])[0]
        classes = self.model.classes_
        best_idx = proba.argmax()
        return classes[best_idx], proba[best_idx]

    # ---------- main entry point ----------

    def translate(self, text):
        """Returns (sql_string, intent, confidence, note). `note` is set when
        a builder made a helpful automatic adjustment worth surfacing (e.g.
        adding a surrogate INT primary key), otherwise None. Raises
        TranslationError if the sentence can't be safely converted by
        either the local classifier or (if configured) the LLM fallback."""
        self.last_note = None
        intent, confidence = self.classify(text)
        local_error = None

        if confidence >= CONFIDENCE_THRESHOLD:
            try:
                sql = self._build_by_intent(intent, text)
                self._sync_schema(sql)
                return sql, intent, confidence, self.last_note
            except TranslationError as e:
                local_error = e
        else:
            local_error = TranslationError(
                f"Not confident enough (intent guess: {intent}, {confidence:.0%})."
            )

        if llm_fallback.is_available():
            try:
                sql = llm_fallback.translate_with_llm(text, self.schemas)
            except RuntimeError as e:
                raise TranslationError(f"{local_error} LLM fallback also failed: {e}")
            if sql:
                self._sync_schema(sql)
                return sql, "LLM_FALLBACK", None, None
            raise TranslationError(
                f"{local_error} The LLM fallback couldn't express this in the "
                f"supported grammar either -- try rephrasing."
            )

        raise TranslationError(
            f"{local_error} Try rephrasing, e.g. 'show all <table>' or "
            f"'create a table called <name> with id int, name varchar 32'. "
            f"(Set GEMINI_API_KEY to enable an LLM fallback for trickier phrasing.)"
        )

    def _build_by_intent(self, intent, text):
        if intent == "CREATE_TABLE":
            return self._build_create(text)
        elif intent == "INSERT":
            return self._build_insert(text)
        elif intent == "SELECT":
            return self._build_select(text)
        elif intent == "DROP_TABLE":
            return self._build_drop_table(text)
        elif intent == "DELETE":
            return self._build_delete(text)
        elif intent == "UPDATE":
            return self._build_update(text)
        elif intent == "RENAME_COLUMN":
            return self._build_rename_column(text)
        elif intent == "SHOW_TABLES":
            return "SHOW TABLES"
        raise TranslationError(f"Unknown intent: {intent}")

    def _sync_schema(self, sql):
        """Keeps self.schemas in sync with any CREATE TABLE or DROP TABLE
        statement, regardless of whether it came from the local builder or
        the LLM fallback -- needed so later INSERTs know column order, and
        so a dropped table can't be inserted/selected into by name anymore."""
        m = re.match(r"\s*CREATE TABLE\s+(\w+)\s*\((.*)\)\s*$", sql, re.IGNORECASE | re.DOTALL)
        if m:
            table, cols_str = m.group(1).lower(), m.group(2)
            columns = []
            for part in cols_str.split(","):
                pm = re.match(r"\s*(\w+)\s+(INT|VARCHAR\s*\(\s*\d+\s*\))", part, re.IGNORECASE)
                if pm:
                    columns.append((pm.group(1).lower(), pm.group(2).upper().replace(" ", "")))
            if columns:
                self.schemas[table] = columns
            return

        m = re.match(r"\s*DROP TABLE\s+(\w+)\s*$", sql, re.IGNORECASE)
        if m:
            table = m.group(1).lower()
            self.schemas.pop(table, None)
            self.auto_id_tables.discard(table)
            return

        m = re.match(
            r"\s*ALTER TABLE\s+(\w+)\s+RENAME COLUMN\s+(\w+)\s+TO\s+(\w+)\s*$",
            sql, re.IGNORECASE,
        )
        if m:
            table, old, new = m.group(1).lower(), m.group(2).lower(), m.group(3).lower()
            if table in self.schemas:
                self.schemas[table] = [
                    (new, ctype) if cname == old else (cname, ctype)
                    for cname, ctype in self.schemas[table]
                ]

    # ---------- CREATE TABLE ----------

    def _build_create(self, text):
        table = self._extract_table_name(text, contexts=["table", "called", "named"])
        if not table:
            raise TranslationError("Couldn't find a table name in that sentence.")

        columns = self._extract_columns(text)
        if not columns:
            raise TranslationError(
                "Couldn't find any columns. Try: '... with id int, name varchar 32'."
            )

        columns, surrogate_added = self._ensure_int_primary_key(columns)
        if surrogate_added:
            self.auto_id_tables.add(table)
        else:
            self.auto_id_tables.discard(table)

        col_sql = ", ".join(f"{name} {ctype}" for name, ctype in columns)
        return f"CREATE TABLE {table} ({col_sql})"

    def _ensure_int_primary_key(self, columns):
        """mini-dbms requires the first column to be INT (it's the B+Tree
        primary key). Rather than making the user always remember to type
        an int column first, fix it up automatically:
          - if some column is already INT, move it to the front
          - otherwise, add a surrogate 'id INT' (or 'row_id' if 'id' is
            already taken by a non-INT column) as the new first column
        Sets self.last_note so the caller can tell the user what happened.
        Returns (columns, surrogate_added) -- surrogate_added is True only
        when a brand-new synthetic id column was invented (not when an
        existing column was merely reordered), since that's the case where
        INSERT needs to auto-fill a value the user never typed.
        """
        if columns[0][1] == "INT":
            return columns, False

        for i, (name, ctype) in enumerate(columns):
            if ctype == "INT":
                promoted = columns[i]
                rest = columns[:i] + columns[i + 1:]
                self.last_note = (
                    f"Moved '{promoted[0]}' to be the first column -- mini-dbms "
                    f"requires the primary key (first column) to be INT."
                )
                return [promoted] + rest, False

        existing_names = {name for name, _ in columns}
        surrogate = "id" if "id" not in existing_names else "row_id"
        self.last_note = (
            f"No INT column was specified, so added an auto-generated "
            f"'{surrogate} INT' primary key column first -- mini-dbms requires "
            f"the primary key to be INT. It'll be filled in automatically on "
            f"INSERT; your other columns are unchanged."
        )
        return [(surrogate, "INT")] + columns, True

    def _extract_columns(self, text):
        """
        Finds patterns like:
            id int
            id as int
            name varchar 32
            name as varchar(32)
        separated by commas or 'and'.
        """
        # normalize separators so we can split on a single token
        cleaned = re.sub(r"\band\b", ",", text, flags=re.IGNORECASE)
        # only look at the part after the first column-ish keyword, to avoid
        # picking up words from "create a table called users with columns"
        after_with = re.split(r"\bwith columns\b|\bwith\b|\bhaving\b", cleaned, maxsplit=1, flags=re.IGNORECASE)
        segment = after_with[-1] if len(after_with) > 1 else cleaned

        parts = [p.strip() for p in segment.split(",") if p.strip()]
        columns = []
        pattern = re.compile(
            r"(?P<name>[a-zA-Z_][a-zA-Z0-9_]*)\s+(?:as\s+)?"
            r"(?P<type>int|integer|varchar\s*\(?\s*\d*\s*\)?|text|string)\b",
            re.IGNORECASE,
        )
        for part in parts:
            m = pattern.search(part)
            if not m:
                continue
            name = m.group("name").lower()
            raw_type = m.group("type").lower().replace(" ", "")
            columns.append((name, self._normalize_type(raw_type)))
        return columns

    @staticmethod
    def _normalize_type(raw_type):
        if raw_type in ("int", "integer"):
            return "INT"
        num_match = re.search(r"\d+", raw_type)
        size = num_match.group(0) if num_match else "32"
        return f"VARCHAR({size})"

    # ---------- INSERT ----------

    def _build_insert(self, text):
        table = self._extract_table_name(
            text, contexts=["to", "into", "in", "for"], prefer_known=True
        )
        if not table:
            raise TranslationError("Couldn't tell which table to insert into.")

        if table not in self.schemas:
            raise TranslationError(
                f"I don't know the schema for '{table}' yet -- create it first "
                f"in this session, or run CREATE TABLE manually."
            )

        # Remove just the "to/into/in/for <table>" phrase itself (it can
        # appear anywhere -- start, middle, or end of the sentence), and
        # any filler word like "having", rather than deleting everything
        # after it (which would wipe out values that come afterward).
        cleaned = re.sub(
            rf"\b(?:to|into|in|for)\s+{re.escape(table)}\b", "", text, flags=re.IGNORECASE
        )
        cleaned = re.sub(r"\bhaving\b", "", cleaned, flags=re.IGNORECASE)
        cleaned = re.sub(r"\s+and\s+", ", ", cleaned, flags=re.IGNORECASE).strip()

        # extract "<col> <value>" pairs, e.g. "id 1, name alice, email a@b.com"
        pairs = {}
        escaped_names = "|".join(re.escape(c) for c, _ in self.schemas[table])
        for col_name, col_type in self.schemas[table]:
            m = re.search(
                rf"\b{re.escape(col_name)}\b\s+(?:is\s+|as\s+|=\s*)?"
                rf"(?P<value>.+?)(?=\s*,|\s+(?:{escaped_names})\b|$)",
                cleaned,
                re.IGNORECASE,
            )
            if m:
                pairs[col_name] = m.group("value").strip().rstrip(",")

        if not pairs:
            raise TranslationError(
                f"Couldn't find values for {table}'s columns "
                f"({', '.join(c for c, _ in self.schemas[table])})."
            )

        values = []
        pk_col = self.schemas[table][0][0]
        for col_name, col_type in self.schemas[table]:
            raw_val = pairs.get(col_name)
            if raw_val is None:
                if col_name == pk_col and table in self.auto_id_tables:
                    values.append(AUTO_ID_PLACEHOLDER)
                    continue
                raise TranslationError(f"Missing a value for column '{col_name}'.")
            if col_type == "INT":
                num_match = re.search(r"-?\d+", raw_val)
                if not num_match:
                    raise TranslationError(f"Expected a number for '{col_name}', got '{raw_val}'.")
                values.append(num_match.group(0))
            else:
                clean_val = raw_val.strip().strip("'\"")
                values.append(f"'{clean_val}'")

        return f"INSERT INTO {table} VALUES ({', '.join(values)})"

    # ---------- SELECT ----------

    def _build_select(self, text):
        table = self._extract_table_name(
            text, contexts=["from", "in", "table", "all", "the"], prefer_known=True
        )
        if not table:
            raise TranslationError("Couldn't tell which table to query.")

        where_match = re.search(
            r"\bid\b\s*(?:=|is|equals?|equal to)?\s*(-?\d+)", text, re.IGNORECASE
        )
        if where_match:
            value = where_match.group(1)
            pk_col = self.schemas.get(table, [("id", "INT")])[0][0]
            return f"SELECT * FROM {table} WHERE {pk_col} = {value}"

        return f"SELECT * FROM {table}"

    # ---------- DROP TABLE ----------

    def _build_drop_table(self, text):
        # prefer_known=True so if the sentence mentions a table we've
        # actually seen created this session, that wins over guessing from
        # prepositions -- important since "delete the pets table" has no
        # "from"/"into" anchor the other builders rely on.
        table = self._extract_table_name(
            text, contexts=["table", "the"], prefer_known=True
        )
        if not table:
            raise TranslationError("Couldn't tell which table to drop.")
        return f"DROP TABLE {table}"

    # ---------- shared: single equality condition ----------

    def _extract_simple_condition(self, text, table):
        """Finds one 'col OP value' condition for a known column of
        `table` -- e.g. 'id is 5', 'name = alice', 'age equals 30'.
        Returns a SQL-ready 'col = value' string, or None. Only handles a
        single equality -- anything with AND/OR, ranges, or other operators
        isn't attempted locally and falls through to the LLM fallback
        instead of risking a wrong guess."""
        if table not in self.schemas:
            return None
        for col_name, col_type in self.schemas[table]:
            m = re.search(
                rf"\b{re.escape(col_name)}\b\s*(?:is|=|equals?|equal to)\s*"
                rf"(?P<value>'[^']*'|\"[^\"]*\"|\S+)",
                text, re.IGNORECASE,
            )
            if not m:
                continue
            raw_val = m.group("value").rstrip(",.")
            if col_type == "INT":
                num_match = re.search(r"-?\d+", raw_val)
                if not num_match:
                    continue
                return f"{col_name} = {num_match.group(0)}"
            clean_val = raw_val.strip().strip("'\"")
            return f"{col_name} = '{clean_val}'"
        return None

    # ---------- DELETE ----------

    def _build_delete(self, text):
        table = self._extract_table_name(
            text, contexts=["from", "in", "table"], prefer_known=True
        )
        if not table:
            raise TranslationError("Couldn't tell which table to delete from.")
        if table not in self.schemas:
            raise TranslationError(
                f"I don't know the schema for '{table}' yet -- create it first "
                f"in this session, or run CREATE TABLE manually."
            )

        cond = self._extract_simple_condition(text, table)
        if cond:
            return f"DELETE FROM {table} WHERE {cond}"

        if re.search(r"\ball\b|\bevery\b|\beverything\b", text, re.IGNORECASE):
            return f"DELETE FROM {table}"

        raise TranslationError(
            f"Couldn't tell which row(s) to delete from '{table}' -- try "
            f"'delete all rows from {table}' or 'delete from {table} where "
            f"<col> is <value>'."
        )

    # ---------- UPDATE ----------

    def _build_update(self, text):
        table = self._extract_table_name(
            text, contexts=["in", "for", "table"], prefer_known=True
        )
        if not table:
            raise TranslationError("Couldn't tell which table to update.")
        if table not in self.schemas:
            raise TranslationError(
                f"I don't know the schema for '{table}' yet -- create it first "
                f"in this session, or run CREATE TABLE manually."
            )

        # Split off any WHERE-ish part first -- otherwise its "col value"
        # pattern would get mistaken for a SET assignment too.
        parts = re.split(r"\bwhere\b", text, maxsplit=1, flags=re.IGNORECASE)
        set_part, where_part = parts[0], (parts[1] if len(parts) > 1 else None)

        pk_col = self.schemas[table][0][0]
        assignments = []
        for col_name, col_type in self.schemas[table]:
            if col_name == pk_col:
                continue  # the engine forbids updating the primary key
            m = re.search(
                rf"\b{re.escape(col_name)}\b\s*(?:to|as|=)\s*"
                rf"(?P<value>'[^']*'|\"[^\"]*\"|\S+)",
                set_part, re.IGNORECASE,
            )
            if not m:
                continue
            raw_val = m.group("value").rstrip(",.")
            if col_type == "INT":
                num_match = re.search(r"-?\d+", raw_val)
                if num_match:
                    assignments.append(f"{col_name} = {num_match.group(0)}")
            else:
                clean_val = raw_val.strip().strip("'\"")
                assignments.append(f"{col_name} = '{clean_val}'")

        if not assignments:
            raise TranslationError(
                f"Couldn't tell what to change on '{table}' -- try 'update "
                f"{table} set <col> to <value> where <col> is <value>'."
            )

        sql = f"UPDATE {table} SET {', '.join(assignments)}"
        if where_part:
            cond = self._extract_simple_condition(where_part, table)
            if not cond:
                raise TranslationError(
                    f"Couldn't understand the WHERE condition in '{where_part.strip()}'."
                )
            sql += f" WHERE {cond}"
        return sql

    # ---------- ALTER TABLE RENAME COLUMN ----------

    def _build_rename_column(self, text):
        table = self._extract_table_name(
            text, contexts=["in", "of", "table"], prefer_known=True
        )
        if not table:
            raise TranslationError("Couldn't tell which table's column to rename.")
        if table not in self.schemas:
            raise TranslationError(
                f"I don't know the schema for '{table}' yet -- create it first "
                f"in this session, or run CREATE TABLE manually."
            )

        m = re.search(
            r"\b(?:rename|change)\b.*?\b(?:column\s+)?"
            r"(?P<old>[a-zA-Z_][a-zA-Z0-9_]*)\s+(?:to|as)\s+"
            r"(?P<new>[a-zA-Z_][a-zA-Z0-9_]*)",
            text, re.IGNORECASE,
        )
        if not m:
            raise TranslationError(
                f"Couldn't parse the rename -- try 'rename column <old> to "
                f"<new> in {table}'."
            )
        old_name, new_name = m.group("old").lower(), m.group("new").lower()
        known_cols = {c for c, _ in self.schemas[table]}
        if old_name not in known_cols:
            raise TranslationError(f"'{old_name}' isn't a column of '{table}'.")
        if old_name == table or new_name == table:
            # the regex can occasionally swallow the table name itself as
            # "old"/"new" for oddly worded sentences -- bail out to the LLM
            # fallback rather than risk renaming the wrong thing
            raise TranslationError("Couldn't reliably parse the column names in that rename.")

        return f"ALTER TABLE {table} RENAME COLUMN {old_name} TO {new_name}"

    # ---------- shared helpers ----------

    # words that never make sense as a table name themselves -- keep
    # scanning past them for the real name
    _FILLER_WORDS = {
        "a", "the", "called", "named", "for", "having", "name", "table",
        "to", "into", "in", "of", "new",
    }
    # words that signal we've run past any table name into the columns/
    # values section -- stop scanning if we hit one of these
    _STOP_WORDS = {"columns", "column", "with", "rows", "records", "entries", "values"}

    def _extract_table_name(self, text, contexts, prefer_known=False):
        words = re.findall(r"[a-zA-Z_][a-zA-Z0-9_]*", text.lower())

        # If we already know some table names, and exactly one of them
        # appears in the sentence, that's almost always correct and is more
        # robust than guessing from prepositions. Also tolerate a singular
        # mention of a plural table name (e.g. "the user" -> table "users").
        if prefer_known and self.schemas:
            mentioned = set()
            for t in self.schemas:
                if t in words:
                    mentioned.add(t)
                elif t.endswith("s") and t[:-1] in words:
                    mentioned.add(t)
            if len(mentioned) == 1:
                return next(iter(mentioned))

        lowered = text.lower()
        for ctx in contexts:
            m = re.search(rf"\b{re.escape(ctx)}\b", lowered)
            if not m:
                continue
            # look at every word after the anchor, skipping filler words,
            # until we find a real candidate or hit a stop word
            rest = lowered[m.end():]
            for word in re.findall(r"[a-zA-Z_][a-zA-Z0-9_]*", rest):
                if word in self._FILLER_WORDS:
                    continue
                if word in self._STOP_WORDS:
                    break
                return word
        return None