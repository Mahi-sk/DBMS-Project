#pragma once
#include <string>
#include "database.h"

// A small, hand-written recursive-descent-ish parser for a SQL subset:
//   CREATE TABLE name (col1 INT, col2 VARCHAR(32), ...)
//   DROP TABLE name
//   SHOW TABLES
//   INSERT INTO name VALUES (1, 'alice', ...)
//   SELECT * FROM name
//   SELECT * FROM name WHERE id = 5
//   UPDATE name SET col = value [, ...] [WHERE ...]
//   DELETE FROM name [WHERE ...]
//   ALTER TABLE name RENAME COLUMN old TO new
// The first column of a table is always treated as its integer primary key.
// A single file (Database) can now hold more than one table.

class Engine {
public:
    // Executes one statement against `db`. Returns human-readable output.
    static std::string execute(Database& db, const std::string& statement);
};