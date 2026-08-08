#include "sql.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace {

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Very small tokenizer: splits on whitespace but keeps parenthesized
// groups and quoted strings intact so callers can split them further.
std::vector<std::string> tokenize(const std::string& s) {
    std::vector<std::string> tokens;
    std::string cur;
    int depth = 0;
    bool inQuote = false;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (inQuote) {
            cur += c;
            if (c == '\'') inQuote = false;
            continue;
        }
        if (c == '\'') {
            inQuote = true;
            cur += c;
            continue;
        }
        if (c == '(') depth++;
        if (c == ')') depth--;
        if (std::isspace(static_cast<unsigned char>(c)) && depth == 0) {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

std::vector<std::string> splitCommaTopLevel(const std::string& s) {
    std::vector<std::string> parts;
    std::string cur;
    int depth = 0;
    bool inQuote = false;
    for (char c : s) {
        if (inQuote) {
            cur += c;
            if (c == '\'') inQuote = false;
            continue;
        }
        if (c == '\'') { inQuote = true; cur += c; continue; }
        if (c == '(') depth++;
        if (c == ')') depth--;
        if (c == ',' && depth == 0) {
            parts.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!trim(cur).empty()) parts.push_back(trim(cur));
    return parts;
}

std::string stripOuterParens(const std::string& s) {
    std::string t = trim(s);
    if (!t.empty() && t.front() == '(' && t.back() == ')') {
        return t.substr(1, t.size() - 2);
    }
    return t;
}

ColumnDef parseColumnDef(const std::string& def) {
    std::string t = trim(def);
    size_t sp = t.find(' ');
    if (sp == std::string::npos) throw std::runtime_error("Invalid column definition: " + def);
    std::string name = t.substr(0, sp);
    std::string typeStr = upper(trim(t.substr(sp + 1)));

    ColumnDef col;
    col.name = name;
    if (typeStr.rfind("INT", 0) == 0) {
        col.type = ColumnType::Int;
        col.size = 4;
    } else if (typeStr.rfind("VARCHAR", 0) == 0) {
        col.type = ColumnType::Varchar;
        size_t lp = typeStr.find('(');
        size_t rp = typeStr.find(')');
        if (lp == std::string::npos || rp == std::string::npos) {
            throw std::runtime_error("VARCHAR requires a size, e.g. VARCHAR(32)");
        }
        col.size = static_cast<uint32_t>(std::stoul(typeStr.substr(lp + 1, rp - lp - 1)));
    } else {
        throw std::runtime_error("Unsupported column type: " + typeStr + " (supported: INT, VARCHAR(n))");
    }
    return col;
}

CellValue parseLiteral(const std::string& lit, ColumnType expected) {
    std::string t = trim(lit);
    if (!t.empty() && t.front() == '\'' && t.back() == '\'') {
        std::string s = t.substr(1, t.size() - 2);
        if (expected != ColumnType::Varchar) {
            throw std::runtime_error("Type mismatch: got string literal for a non-VARCHAR column");
        }
        return s;
    }
    if (expected != ColumnType::Int) {
        throw std::runtime_error("Type mismatch: expected a quoted string for VARCHAR column");
    }
    return static_cast<int32_t>(std::stol(t));
}

std::string formatCell(const CellValue& v) {
    if (std::holds_alternative<int32_t>(v)) return std::to_string(std::get<int32_t>(v));
    return std::get<std::string>(v);
}

// Case-insensitive find of a whole keyword (surrounded by non-alnum or
// string boundary) in `s`, returning its position in `s` (not upperS).
// Since toupper() never changes character count for ASCII, indices in
// upperS line up 1:1 with indices in the original string s.
size_t findKeywordCI(const std::string& s, const std::string& upperS, const std::string& keywordUpper) {
    size_t pos = 0;
    while ((pos = upperS.find(keywordUpper, pos)) != std::string::npos) {
        bool leftOk = (pos == 0) || !std::isalnum(static_cast<unsigned char>(s[pos - 1]));
        size_t endPos = pos + keywordUpper.size();
        bool rightOk = (endPos >= s.size()) || !std::isalnum(static_cast<unsigned char>(s[endPos]));
        if (leftOk && rightOk) return pos;
        pos += 1;
    }
    return std::string::npos;
}

struct Condition {
    std::string col;
    std::string op; // "=", "!=", "<", ">", "<=", ">="
    std::string valueLit;
};

// Splits `s` on a case-insensitive whole-word separator (" AND " / " OR "),
// respecting quoted strings so a literal like 'A AND B' isn't split.
std::vector<std::string> splitOnKeywordCI(const std::string& s, const std::string& sepUpper) {
    std::string upperS = upper(s);
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t pos = findKeywordCI(s.substr(start), upperS.substr(start), sepUpper);
        if (pos == std::string::npos) {
            parts.push_back(trim(s.substr(start)));
            break;
        }
        parts.push_back(trim(s.substr(start, pos)));
        start += pos + sepUpper.size();
    }
    return parts;
}

Condition parseSingleCondition(const std::string& clause) {
    // Longest operators checked first so "<=" isn't mistaken for "<" + "=".
    static const std::vector<std::string> ops = {"!=", "<>", "<=", ">=", "=", "<", ">"};
    for (auto& op : ops) {
        size_t pos = clause.find(op);
        if (pos == std::string::npos) continue;
        Condition c;
        c.col = trim(clause.substr(0, pos));
        c.op = (op == "<>") ? "!=" : op;
        c.valueLit = trim(clause.substr(pos + op.size()));
        return c;
    }
    throw std::runtime_error("Could not find a comparison operator in WHERE clause: " + clause);
}

// Parses a WHERE clause into conditions + how they're combined ("AND",
// "OR", or "" for a single condition). Mixing AND and OR in one clause
// isn't supported -- pick one, to keep evaluation unambiguous without a
// full expression parser/precedence rules.
std::vector<Condition> parseConditions(const std::string& whereClause, std::string& logicOut) {
    std::string upperWhere = upper(whereClause);
    bool hasAnd = findKeywordCI(whereClause, upperWhere, "AND") != std::string::npos;
    bool hasOr = findKeywordCI(whereClause, upperWhere, "OR") != std::string::npos;
    if (hasAnd && hasOr) {
        throw std::runtime_error("Mixing AND and OR in one WHERE clause isn't supported -- use only one");
    }
    logicOut = hasAnd ? "AND" : (hasOr ? "OR" : "");

    std::vector<std::string> parts;
    if (hasAnd) parts = splitOnKeywordCI(whereClause, "AND");
    else if (hasOr) parts = splitOnKeywordCI(whereClause, "OR");
    else parts = {whereClause};

    std::vector<Condition> conds;
    for (auto& p : parts) conds.push_back(parseSingleCondition(p));
    return conds;
}

// -1 / 0 / 1, comparing same-typed CellValues.
int compareCell(const CellValue& a, const CellValue& b) {
    if (std::holds_alternative<int32_t>(a)) {
        int32_t x = std::get<int32_t>(a), y = std::get<int32_t>(b);
        return (x < y) ? -1 : (x > y ? 1 : 0);
    }
    int c = std::get<std::string>(a).compare(std::get<std::string>(b));
    return (c < 0) ? -1 : (c > 0 ? 1 : 0);
}

bool applyOp(int cmp, const std::string& op) {
    if (op == "=") return cmp == 0;
    if (op == "!=") return cmp != 0;
    if (op == "<") return cmp < 0;
    if (op == ">") return cmp > 0;
    if (op == "<=") return cmp <= 0;
    if (op == ">=") return cmp >= 0;
    throw std::runtime_error("Unsupported operator: " + op);
}

bool rowMatches(const Row& row, const std::vector<ColumnDef>& cols,
                const std::vector<Condition>& conds, const std::string& logic) {
    std::vector<bool> results;
    for (auto& c : conds) {
        int idx = -1;
        for (size_t i = 0; i < cols.size(); i++) {
            if (cols[i].name == c.col) { idx = static_cast<int>(i); break; }
        }
        if (idx < 0) throw std::runtime_error("Unknown column in WHERE: " + c.col);
        CellValue litVal = parseLiteral(c.valueLit, cols[idx].type);
        results.push_back(applyOp(compareCell(row[idx], litVal), c.op));
    }
    if (results.empty()) return true;
    if (logic == "OR") {
        for (bool r : results) if (r) return true;
        return false;
    }
    for (bool r : results) if (!r) return false; // AND, or a single condition
    return true;
}

// A WHERE clause can hit the tree's fast O(log n) point lookup only when
// it's exactly one equality condition on the primary key. Anything else
// (other columns, ranges, AND/OR) falls back to a full table scan since
// this mini-engine has no secondary indexes.
bool isPointLookup(const std::vector<Condition>& conds, const std::string& pkName) {
    return conds.size() == 1 && conds[0].op == "=" && conds[0].col == pkName;
}

} // namespace

std::string Engine::execute(Database& db, const std::string& statementRaw) {
    std::string statement = trim(statementRaw);
    if (statement.empty()) return "";
    if (statement.back() == ';') statement.pop_back();
    statement = trim(statement);

    std::string upperStmt = upper(statement);

    if (upperStmt.rfind("CREATE TABLE", 0) == 0) {
        // CREATE TABLE name (col TYPE, col TYPE, ...)
        std::string rest = trim(statement.substr(std::string("CREATE TABLE").size()));
        size_t parenPos = rest.find('(');
        if (parenPos == std::string::npos) throw std::runtime_error("Expected '(' after table name");
        std::string tableName = trim(rest.substr(0, parenPos));
        std::string colsStr = stripOuterParens(rest.substr(parenPos));

        std::vector<ColumnDef> cols;
        for (auto& part : splitCommaTopLevel(colsStr)) {
            cols.push_back(parseColumnDef(part));
        }
        if (cols.empty()) throw std::runtime_error("CREATE TABLE needs at least one column");
        if (cols[0].type != ColumnType::Int) {
            throw std::runtime_error("First column must be INT (it is used as the primary key)");
        }

        db.createTable(tableName, cols);
        return "Table '" + tableName + "' created (" + std::to_string(cols.size()) + " columns, "
               + "primary key: " + cols[0].name + ").";
    }

    if (upperStmt.rfind("DROP TABLE", 0) == 0) {
        std::string tableName = trim(statement.substr(std::string("DROP TABLE").size()));
        if (tableName.empty()) throw std::runtime_error("Usage: DROP TABLE name");
        db.dropTable(tableName);
        return "Table '" + tableName + "' dropped.";
    }

    if (upperStmt == "SHOW TABLES") {
        auto names = db.listTables();
        if (names.empty()) return "(no tables yet)";
        std::ostringstream out;
        for (size_t i = 0; i < names.size(); i++) {
            out << names[i];
            if (i + 1 < names.size()) out << "\n";
        }
        return out.str();
    }

    if (upperStmt.rfind("INSERT INTO", 0) == 0) {
        // INSERT INTO table VALUES (...)
        std::string rest = trim(statement.substr(std::string("INSERT INTO").size()));
        size_t valuesPos = upper(rest).find("VALUES");
        if (valuesPos == std::string::npos) throw std::runtime_error("Expected VALUES(...) in INSERT statement");
        std::string tableName = trim(rest.substr(0, valuesPos));
        Table& table = db.getTable(tableName);

        std::string valuesStr = stripOuterParens(trim(rest.substr(valuesPos + 6)));
        auto parts = splitCommaTopLevel(valuesStr);
        const auto& cols = table.columns();
        if (parts.size() != cols.size()) {
            throw std::runtime_error("Column count mismatch: table has " + std::to_string(cols.size())
                                      + " columns, got " + std::to_string(parts.size()) + " values");
        }
        Row row;
        for (size_t i = 0; i < parts.size(); i++) {
            row.push_back(parseLiteral(parts[i], cols[i].type));
        }
        table.insertRow(row);
        return "1 row inserted.";
    }

    if (upperStmt.rfind("SELECT", 0) == 0) {
        // SELECT * FROM table [WHERE <conditions>]
        std::string rest = trim(statement.substr(std::string("SELECT").size()));
        std::string upperRest = upper(rest);
        size_t fromPos = findKeywordCI(rest, upperRest, "FROM");
        if (fromPos == std::string::npos) throw std::runtime_error("Expected FROM in SELECT statement");
        std::string afterFrom = trim(rest.substr(fromPos + 4));
        std::string upperAfterFrom = upper(afterFrom);
        size_t wherePos = findKeywordCI(afterFrom, upperAfterFrom, "WHERE");
        std::string tableName = (wherePos == std::string::npos) ? afterFrom : trim(afterFrom.substr(0, wherePos));
        if (tableName.empty()) throw std::runtime_error("Expected a table name after FROM");

        Table& table = db.getTable(tableName);
        const auto& cols = table.columns();

        std::vector<Row> rows;
        if (wherePos == std::string::npos) {
            rows = table.selectAll();
        } else {
            std::string whereClause = trim(afterFrom.substr(wherePos + 5));
            std::string logic;
            auto conds = parseConditions(whereClause, logic);

            if (isPointLookup(conds, cols[0].name)) {
                int32_t key = std::get<int32_t>(parseLiteral(conds[0].valueLit, ColumnType::Int));
                Row r;
                if (table.selectByKey(key, r)) rows.push_back(r);
            } else {
                for (auto& r : table.selectAll()) {
                    if (rowMatches(r, cols, conds, logic)) rows.push_back(r);
                }
            }
        }

        std::ostringstream out;
        for (size_t i = 0; i < cols.size(); i++) {
            out << cols[i].name;
            if (i + 1 < cols.size()) out << " | ";
        }
        out << "\n";
        for (auto& r : rows) {
            for (size_t i = 0; i < r.size(); i++) {
                out << formatCell(r[i]);
                if (i + 1 < r.size()) out << " | ";
            }
            out << "\n";
        }
        out << "(" << rows.size() << " row" << (rows.size() == 1 ? "" : "s") << ")";
        return out.str();
    }

    if (upperStmt.rfind("DELETE FROM", 0) == 0 || upperStmt.rfind("DELETE", 0) == 0) {
        // DELETE FROM table [WHERE <conditions>]
        std::string rest = upperStmt.rfind("DELETE FROM", 0) == 0
            ? trim(statement.substr(std::string("DELETE FROM").size()))
            : trim(statement.substr(std::string("DELETE").size()));
        std::string upperRest = upper(rest);
        size_t wherePos = findKeywordCI(rest, upperRest, "WHERE");

        std::string tableName = (wherePos == std::string::npos) ? rest : trim(rest.substr(0, wherePos));
        if (tableName.empty()) throw std::runtime_error("Expected a table name after DELETE FROM");
        Table& table = db.getTable(tableName);
        const auto& cols = table.columns();
        size_t deleted = 0;

        if (wherePos == std::string::npos) {
            for (auto& r : table.selectAll()) {
                if (table.deleteByKey(std::get<int32_t>(r[0]))) deleted++;
            }
        } else {
            std::string whereClause = trim(rest.substr(wherePos + 5));
            std::string logic;
            auto conds = parseConditions(whereClause, logic);

            if (isPointLookup(conds, cols[0].name)) {
                int32_t key = std::get<int32_t>(parseLiteral(conds[0].valueLit, ColumnType::Int));
                if (table.deleteByKey(key)) deleted++;
            } else {
                std::vector<int32_t> keysToDelete;
                for (auto& r : table.selectAll()) {
                    if (rowMatches(r, cols, conds, logic)) keysToDelete.push_back(std::get<int32_t>(r[0]));
                }
                for (int32_t key : keysToDelete) {
                    if (table.deleteByKey(key)) deleted++;
                }
            }
        }
        return std::to_string(deleted) + " row" + (deleted == 1 ? "" : "s") + " deleted.";
    }

    if (upperStmt.rfind("UPDATE", 0) == 0) {
        // UPDATE table SET col = value [, col = value ...] [WHERE <conditions>]
        std::string rest = trim(statement.substr(std::string("UPDATE").size()));
        std::string upperRest = upper(rest);
        size_t setPos = findKeywordCI(rest, upperRest, "SET");
        if (setPos == std::string::npos) throw std::runtime_error("Expected SET in UPDATE statement");
        std::string tableName = trim(rest.substr(0, setPos));
        if (tableName.empty()) throw std::runtime_error("Expected a table name after UPDATE");
        Table& table = db.getTable(tableName);

        std::string afterSet = trim(rest.substr(setPos + 3));

        size_t wherePos = findKeywordCI(afterSet, upper(afterSet), "WHERE");
        std::string setClause = (wherePos == std::string::npos) ? afterSet : trim(afterSet.substr(0, wherePos));

        const auto& cols = table.columns();
        std::vector<std::pair<size_t, std::string>> assignments; // column index, literal
        for (auto& part : splitCommaTopLevel(setClause)) {
            size_t eq = part.find('=');
            if (eq == std::string::npos) throw std::runtime_error("Invalid SET assignment: " + part);
            std::string colName = trim(part.substr(0, eq));
            std::string lit = trim(part.substr(eq + 1));
            size_t idx = std::string::npos;
            for (size_t i = 0; i < cols.size(); i++) {
                if (cols[i].name == colName) { idx = i; break; }
            }
            if (idx == std::string::npos) throw std::runtime_error("Unknown column in SET: " + colName);
            if (idx == 0) throw std::runtime_error("Cannot UPDATE the primary key column ('" + cols[0].name + "')");
            assignments.push_back({idx, lit});
        }

        std::vector<Row> rowsToUpdate;
        if (wherePos == std::string::npos) {
            rowsToUpdate = table.selectAll();
        } else {
            std::string whereClause = trim(afterSet.substr(wherePos + 5));
            std::string logic;
            auto conds = parseConditions(whereClause, logic);
            if (isPointLookup(conds, cols[0].name)) {
                int32_t key = std::get<int32_t>(parseLiteral(conds[0].valueLit, ColumnType::Int));
                Row r;
                if (table.selectByKey(key, r)) rowsToUpdate.push_back(r);
            } else {
                for (auto& r : table.selectAll()) {
                    if (rowMatches(r, cols, conds, logic)) rowsToUpdate.push_back(r);
                }
            }
        }

        size_t updated = 0;
        for (auto row : rowsToUpdate) { // copy -- we mutate before writing back
            int32_t key = std::get<int32_t>(row[0]);
            for (auto& a : assignments) {
                row[a.first] = parseLiteral(a.second, cols[a.first].type);
            }
            if (table.updateByKey(key, row)) updated++;
        }
        return std::to_string(updated) + " row" + (updated == 1 ? "" : "s") + " updated.";
    }

    if (upperStmt.rfind("ALTER TABLE", 0) == 0) {
        // ALTER TABLE table RENAME COLUMN old TO new
        std::string rest = trim(statement.substr(std::string("ALTER TABLE").size()));
        std::string upperRest = upper(rest);
        size_t renamePos = findKeywordCI(rest, upperRest, "RENAME COLUMN");
        if (renamePos == std::string::npos) {
            throw std::runtime_error("Only ALTER TABLE ... RENAME COLUMN ... TO ... is supported");
        }
        std::string tableName = trim(rest.substr(0, renamePos));
        if (tableName.empty()) throw std::runtime_error("Expected a table name after ALTER TABLE");
        Table& table = db.getTable(tableName);

        std::string afterRename = trim(rest.substr(renamePos + std::string("RENAME COLUMN").size()));
        std::string upperAfter = upper(afterRename);
        size_t toPos = findKeywordCI(afterRename, upperAfter, "TO");
        if (toPos == std::string::npos) throw std::runtime_error("Expected TO in RENAME COLUMN");

        std::string oldName = trim(afterRename.substr(0, toPos));
        std::string newName = trim(afterRename.substr(toPos + 2));
        if (oldName.empty() || newName.empty()) {
            throw std::runtime_error("Usage: ALTER TABLE t RENAME COLUMN old TO new");
        }
        table.renameColumn(oldName, newName);
        return "Column '" + oldName + "' renamed to '" + newName + "'.";
    }

    throw std::runtime_error("Unrecognized statement. Supported: CREATE TABLE, DROP TABLE, SHOW TABLES, "
                              "INSERT INTO, SELECT, UPDATE, DELETE FROM, ALTER TABLE ... RENAME COLUMN.");
}