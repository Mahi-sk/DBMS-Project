#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include "pager.h"
#include "btree.h"

enum class ColumnType : uint8_t { Int = 0, Varchar = 1 };

struct ColumnDef {
    std::string name;
    ColumnType type;
    uint32_t size; // bytes: 4 for Int, N for Varchar(N)
};

using CellValue = std::variant<int32_t, std::string>;
using Row = std::vector<CellValue>;

// Per-table metadata page layout (page number given by the Database's
// catalog -- see database.h -- rather than always being page 0, now that
// a single file can hold more than one table):
//   [0..63]    table name (fixed 64 bytes, null padded)
//   [64..67]   num_columns (uint32)
//   [68.. ]    column entries, each: name(32 bytes) + type(1 byte) + size(uint32)
//   ...        root_page_num (uint32), immediately after the last column entry
constexpr uint32_t META_TABLE_NAME_SIZE = 64;
constexpr uint32_t META_COLUMN_NAME_SIZE = 32;
constexpr uint32_t META_COLUMN_ENTRY_SIZE = META_COLUMN_NAME_SIZE + 1 + 4;

class Table {
public:
    // "Load existing" mode: a table's schema already lives at `metaPage`
    // (Database looked this page number up in the catalog).
    Table(Pager& pager, uint32_t metaPage);
    // "About to create" mode: caller must call createTable() next with the
    // metaPage/rootPage Database has already allocated for this table.
    explicit Table(Pager& pager);
    ~Table();

    bool exists() const { return schemaLoaded_; }

    void createTable(uint32_t metaPage, uint32_t rootPage, const std::string& tableName,
                      const std::vector<ColumnDef>& columns);

    void insertRow(const Row& row);
    std::vector<Row> selectAll();
    bool selectByKey(int32_t key, Row& outRow);

    // Returns false if no row with that key existed.
    bool deleteByKey(int32_t key);

    // Overwrites the row stored at `key` with `newRow` (same primary key --
    // this does not move the row in the tree, just rewrites its bytes in
    // place). Returns false if no row with that key existed.
    bool updateByKey(int32_t key, const Row& newRow);

    // Renames a column in the schema. Row layout/data is untouched since
    // only the column *name* changes, not its type/size/position.
    void renameColumn(const std::string& oldName, const std::string& newName);

    const std::vector<ColumnDef>& columns() const { return columns_; }
    const std::string& tableName() const { return tableName_; }
    uint32_t rowSize() const { return rowSize_; }

private:
    void loadSchema();
    void saveSchema();
    void computeRowSize();

    Row deserializeRow(const char* buf) const;
    void serializeRow(const Row& row, char* out) const;

    Pager& pager_;
    uint32_t metaPage_ = 0;
    std::string tableName_;
    std::vector<ColumnDef> columns_;
    uint32_t rowSize_ = 0;
    uint32_t rootPage_ = 0;
    bool schemaLoaded_ = false;
};