#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "pager.h"
#include "table.h"

// Page 0 of the file is now a small catalog mapping table name -> the
// page number where that table's own metadata (schema + root page) lives.
// This is what makes multiple tables possible: previously page 0 *was*
// the one table's metadata, so a second CREATE TABLE just overwrote it.
//
// Catalog page (page 0) layout:
//   [0..3]     num_tables (uint32)
//   [4.. ]     entries, each: table_name(64 bytes, null padded) + meta_page_num(uint32)
constexpr uint32_t CATALOG_NAME_SIZE = 64;
constexpr uint32_t CATALOG_ENTRY_SIZE = CATALOG_NAME_SIZE + 4;
constexpr uint32_t MAX_CATALOG_ENTRIES = (PAGE_SIZE - 4) / CATALOG_ENTRY_SIZE;

class Database {
public:
    explicit Database(const std::string& filename);
    ~Database();

    bool tableExists(const std::string& name) const;
    std::vector<std::string> listTables() const;

    // Throws if a table with this name already exists.
    Table& createTable(const std::string& name, const std::vector<ColumnDef>& columns);

    // Throws if no table with this name exists.
    Table& getTable(const std::string& name);

    // Throws if no table with this name exists. Removes the catalog entry;
    // note the pages that table used (its metadata page, root page, and any
    // pages from B+Tree splits) are NOT reclaimed -- same kind of documented
    // simplification as the engine's lack of internal-node splitting. A real
    // engine would track a free-page list to recycle them.
    void dropTable(const std::string& name);

private:
    void loadCatalog();
    void saveCatalog();

    Pager pager_;
    std::vector<std::pair<std::string, uint32_t>> catalog_; // name -> metaPage
    std::map<std::string, std::unique_ptr<Table>> openTables_; // lazily opened/created
};