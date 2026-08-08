#include "database.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

Database::Database(const std::string& filename) : pager_(filename) {
    if (pager_.numPages() == 0) {
        // Brand-new file: claim page 0 for the catalog and write an
        // empty one (numTables = 0).
        pager_.allocatePage();
        saveCatalog();
    } else {
        loadCatalog();
    }
}

Database::~Database() {
    // Each open Table saves its own schema in its destructor; dropping
    // openTables_ runs those destructors before we flush everything once.
    openTables_.clear();
    pager_.flushAll();
}

void Database::loadCatalog() {
    char* page = pager_.getPage(0);
    uint32_t numTables;
    memcpy(&numTables, page, 4);

    catalog_.clear();
    uint32_t offset = 4;
    for (uint32_t i = 0; i < numTables; i++) {
        char nameBuf[CATALOG_NAME_SIZE + 1] = {0};
        memcpy(nameBuf, page + offset, CATALOG_NAME_SIZE);
        uint32_t metaPage;
        memcpy(&metaPage, page + offset + CATALOG_NAME_SIZE, 4);
        catalog_.push_back({std::string(nameBuf), metaPage});
        offset += CATALOG_ENTRY_SIZE;
    }
}

void Database::saveCatalog() {
    char* page = pager_.getPage(0);
    memset(page, 0, PAGE_SIZE);
    uint32_t numTables = static_cast<uint32_t>(catalog_.size());
    memcpy(page, &numTables, 4);

    uint32_t offset = 4;
    for (auto& entry : catalog_) {
        memcpy(page + offset, entry.first.c_str(),
               std::min(entry.first.size(), (size_t)CATALOG_NAME_SIZE));
        memcpy(page + offset + CATALOG_NAME_SIZE, &entry.second, 4);
        offset += CATALOG_ENTRY_SIZE;
    }
}

bool Database::tableExists(const std::string& name) const {
    for (auto& entry : catalog_) {
        if (entry.first == name) return true;
    }
    return false;
}

std::vector<std::string> Database::listTables() const {
    std::vector<std::string> names;
    for (auto& entry : catalog_) names.push_back(entry.first);
    return names;
}

Table& Database::createTable(const std::string& name, const std::vector<ColumnDef>& columns) {
    if (tableExists(name)) {
        throw std::runtime_error("Table '" + name + "' already exists");
    }
    if (catalog_.size() >= MAX_CATALOG_ENTRIES) {
        throw std::runtime_error("Catalog is full -- cannot create more tables");
    }

    uint32_t metaPage = pager_.allocatePage();
    uint32_t rootPage = pager_.allocatePage();

    auto table = std::make_unique<Table>(pager_);
    table->createTable(metaPage, rootPage, name, columns);

    catalog_.push_back({name, metaPage});
    saveCatalog();

    Table& ref = *table;
    openTables_[name] = std::move(table);
    return ref;
}

Table& Database::getTable(const std::string& name) {
    auto it = openTables_.find(name);
    if (it != openTables_.end()) return *it->second;

    for (auto& entry : catalog_) {
        if (entry.first == name) {
            auto table = std::make_unique<Table>(pager_, entry.second);
            Table& ref = *table;
            openTables_[name] = std::move(table);
            return ref;
        }
    }
    throw std::runtime_error("Table '" + name + "' does not exist");
}

void Database::dropTable(const std::string& name) {
    if (!tableExists(name)) {
        throw std::runtime_error("Table '" + name + "' does not exist");
    }
    // Erase from the open-table cache first (before it could try to save
    // its own schema on destruction into a catalog slot we're removing).
    openTables_.erase(name);
    catalog_.erase(
        std::remove_if(catalog_.begin(), catalog_.end(),
                        [&](const std::pair<std::string, uint32_t>& e) { return e.first == name; }),
        catalog_.end());
    saveCatalog();
}