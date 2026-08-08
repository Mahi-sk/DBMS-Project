"""
Optional LLM fallback for when the local classifier/slot-filler can't
confidently handle a sentence.

Only activates if the GEMINI_API_KEY environment variable is set --
otherwise the project runs fully offline, as before. Uses only the
standard library (urllib), so it doesn't add a new pip dependency for
people who never turn this on.

Uses Google AI Studio's free Gemini API (no credit card required):
https://aistudio.google.com -> "Get API key"
"""

import json
import os
import urllib.error
import urllib.request

MODEL = "gemini-3.5-flash"
API_URL = "https://generativelanguage.googleapis.com/v1beta/interactions"

SYSTEM_PROMPT = """You translate a natural-language database request into exactly one SQL statement for a minimal SQL engine.
Supported grammar ONLY -- never use anything outside this:
    CREATE TABLE name (col TYPE, col TYPE, ...)      -- TYPE is INT or VARCHAR(n); first column is always the primary key
    DROP TABLE name
    SHOW TABLES
    INSERT INTO name VALUES (v1, v2, ...)             -- values in column order, strings single-quoted
    SELECT * FROM name
    SELECT * FROM name WHERE <conditions>
    UPDATE name SET col = value [, col = value ...] [WHERE <conditions>]   -- cannot SET the primary key column
    DELETE FROM name [WHERE <conditions>]
    ALTER TABLE name RENAME COLUMN old TO new

<conditions> is one or more "col OP value" comparisons joined ONLY by AND or by OR (never both in the same clause).
OP is one of: = != < > <= >=

There is no JOIN, no aggregate functions (COUNT/SUM/AVG), and no subqueries -- never generate them.
If the request truly cannot be expressed in this grammar, respond with exactly: UNSUPPORTED

Respond with ONLY the SQL statement (or UNSUPPORTED). No explanation, no markdown, no backticks."""


def is_available():
    return bool(os.environ.get("GEMINI_API_KEY"))


def translate_with_llm(text, schemas):
    """Returns a SQL string, or None if the model couldn't produce one.
    Raises RuntimeError on a network/API failure."""
    api_key = os.environ.get("GEMINI_API_KEY")
    if not api_key:
        return None

    schema_desc = "\n".join(
        f"  {table}({', '.join(f'{c} {t}' for c, t in cols)})"
        for table, cols in schemas.items()
    ) or "  (no tables created yet)"

    user_prompt = f"Known tables and columns:\n{schema_desc}\n\nRequest: {text}"

    payload = {
        "model": MODEL,
        "input": user_prompt,
        "system_instruction": SYSTEM_PROMPT,
    }

    req = urllib.request.Request(
        API_URL,
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "x-goog-api-key": api_key,
        },
        method="POST",
    )

    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {e.code}: {body[:500]}")
    except urllib.error.URLError as e:
        raise RuntimeError(str(e))

    # Response shape: {"steps": [{"type": "model_output", "content": [{"type": "text", "text": "..."}]}, ...]}
    sql = None
    for step in data.get("steps", []):
        if step.get("type") == "model_output":
            texts = [c.get("text", "") for c in step.get("content", []) if c.get("type") == "text"]
            if texts:
                sql = "".join(texts)

    if not sql:
        return None
    sql = sql.strip().strip("`").strip()

    if not sql or sql.upper() == "UNSUPPORTED":
        return None
    return sql