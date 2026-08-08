#include "table.h"
#include <cstring>
#include <stdexcept>

Table::Table(Pager& pager, uint32_t metaPage) : pager_(pager), metaPage_(metaPage) {
    loadSchema();
}

Table::Table(Pager& pager) : pager_(pager) {
    // Create mode: schema isn't loaded yet -- caller must call
    // createTable() with the metaPage/rootPage Database allocated.
}

Table::~Table() {
    // Note: does NOT flush the pager -- multiple tables now share one
    // Pager (one per Database), so Database::~Database() flushes once,
    // after all its open Tables have saved their own schema below.
    if (schemaLoaded_) {
        saveSchema();
    }
}

void Table::computeRowSize() {
    rowSize_ = 0;
    for (auto& c : columns_) rowSize_ += c.size;
}

void Table::loadSchema() {
    char* meta = pager_.getPage(metaPage_);

    uint32_t numColumns;
    memcpy(&numColumns, meta + META_TABLE_NAME_SIZE, 4);
    if (numColumns == 0 || numColumns > 64) {
        schemaLoaded_ = false;
        return;
    }

    char nameBuf[META_TABLE_NAME_SIZE + 1] = {0};
    memcpy(nameBuf, meta, META_TABLE_NAME_SIZE);
    tableName_ = std::string(nameBuf);

    columns_.clear();
    uint32_t offset = META_TABLE_NAME_SIZE + 4;
    for (uint32_t i = 0; i < numColumns; i++) {
        char colName[META_COLUMN_NAME_SIZE + 1] = {0};
        memcpy(colName, meta + offset, META_COLUMN_NAME_SIZE);
        uint8_t type = static_cast<uint8_t>(meta[offset + META_COLUMN_NAME_SIZE]);
        uint32_t size;
        memcpy(&size, meta + offset + META_COLUMN_NAME_SIZE + 1, 4);
        columns_.push_back({std::string(colName), static_cast<ColumnType>(type), size});
        offset += META_COLUMN_ENTRY_SIZE;
    }
    memcpy(&rootPage_, meta + offset, 4);

    computeRowSize();
    schemaLoaded_ = true;
}

void Table::saveSchema() {
    char* meta = pager_.getPage(metaPage_);
    memset(meta, 0, PAGE_SIZE);
    memcpy(meta, tableName_.c_str(), std::min(tableName_.size(), (size_t)META_TABLE_NAME_SIZE));
    uint32_t numColumns = static_cast<uint32_t>(columns_.size());
    memcpy(meta + META_TABLE_NAME_SIZE, &numColumns, 4);

    uint32_t offset = META_TABLE_NAME_SIZE + 4;
    for (auto& c : columns_) {
        memcpy(meta + offset, c.name.c_str(), std::min(c.name.size(), (size_t)META_COLUMN_NAME_SIZE));
        meta[offset + META_COLUMN_NAME_SIZE] = static_cast<char>(static_cast<uint8_t>(c.type));
        memcpy(meta + offset + META_COLUMN_NAME_SIZE + 1, &c.size, 4);
        offset += META_COLUMN_ENTRY_SIZE;
    }
    memcpy(meta + offset, &rootPage_, 4);
}

void Table::createTable(uint32_t metaPage, uint32_t rootPage, const std::string& tableName,
                         const std::vector<ColumnDef>& cols) {
    if (schemaLoaded_) {
        throw std::runtime_error("Table already exists in this file");
    }
    // Database has already allocated both pages for us (it owns the
    // shared Pager and the catalog, so it decides page numbers).
    metaPage_ = metaPage;
    rootPage_ = rootPage;
    tableName_ = tableName;
    columns_ = cols;
    computeRowSize();

    char* root = pager_.getPage(rootPage_);
    initializeLeaf(root);
    setNodeRoot(root, true);

    schemaLoaded_ = true;
    saveSchema();
}

void Table::serializeRow(const Row& row, char* out) const {
    uint32_t offset = 0;
    for (size_t i = 0; i < columns_.size(); i++) {
        const ColumnDef& col = columns_[i];
        if (col.type == ColumnType::Int) {
            int32_t v = std::get<int32_t>(row[i]);
            memcpy(out + offset, &v, 4);
        } else {
            const std::string& s = std::get<std::string>(row[i]);
            memset(out + offset, 0, col.size);
            memcpy(out + offset, s.c_str(), std::min(s.size(), (size_t)col.size - 1));
        }
        offset += col.size;
    }
}

Row Table::deserializeRow(const char* buf) const {
    Row row;
    uint32_t offset = 0;
    for (auto& col : columns_) {
        if (col.type == ColumnType::Int) {
            int32_t v;
            memcpy(&v, buf + offset, 4);
            row.push_back(v);
        } else {
            std::string s(buf + offset, strnlen(buf + offset, col.size));
            row.push_back(s);
        }
        offset += col.size;
    }
    return row;
}

void Table::insertRow(const Row& row) {
    if (row.empty() || !std::holds_alternative<int32_t>(row[0])) {
        throw std::runtime_error("First column (primary key) must be an INT");
    }
    int32_t key = std::get<int32_t>(row[0]);

    std::vector<char> buf(rowSize_);
    serializeRow(row, buf.data());

    BTree tree(pager_, rowSize_);
    tree.insert(rootPage_, static_cast<uint32_t>(key), buf.data());
}

std::vector<Row> Table::selectAll() {
    std::vector<Row> results;
    BTree tree(pager_, rowSize_);
    BTree::Cursor c = tree.tableStart(rootPage_);

    while (!c.endOfTable) {
        char* node = pager_.getPage(c.pageNum);
        uint32_t numCells = *leafNumCells(node);
        if (c.cellNum >= numCells) {
            uint32_t next = *leafNextLeaf(node);
            if (next == 0) break;
            c.pageNum = next;
            c.cellNum = 0;
            continue;
        }
        results.push_back(deserializeRow(leafValue(node, c.cellNum, rowSize_)));
        c.cellNum++;
    }
    return results;
}

bool Table::selectByKey(int32_t key, Row& outRow) {
    BTree tree(pager_, rowSize_);
    BTree::Cursor c = tree.findKey(rootPage_, static_cast<uint32_t>(key));
    char* node = pager_.getPage(c.pageNum);
    if (c.cellNum >= *leafNumCells(node)) return false;
    if (*leafKey(node, c.cellNum, rowSize_) != static_cast<uint32_t>(key)) return false;
    outRow = deserializeRow(leafValue(node, c.cellNum, rowSize_));
    return true;
}

bool Table::deleteByKey(int32_t key) {
    BTree tree(pager_, rowSize_);
    return tree.remove(rootPage_, static_cast<uint32_t>(key));
}

bool Table::updateByKey(int32_t key, const Row& newRow) {
    BTree tree(pager_, rowSize_);
    BTree::Cursor c = tree.findKey(rootPage_, static_cast<uint32_t>(key));
    char* node = pager_.getPage(c.pageNum);
    if (c.cellNum >= *leafNumCells(node)) return false;
    if (*leafKey(node, c.cellNum, rowSize_) != static_cast<uint32_t>(key)) return false;

    std::vector<char> buf(rowSize_);
    serializeRow(newRow, buf.data());
    memcpy(leafValue(node, c.cellNum, rowSize_), buf.data(), rowSize_);
    return true;
}

void Table::renameColumn(const std::string& oldName, const std::string& newName) {
    bool found = false;
    for (auto& c : columns_) {
        if (c.name == newName && c.name != oldName) {
            throw std::runtime_error("Column '" + newName + "' already exists");
        }
    }
    for (auto& c : columns_) {
        if (c.name == oldName) {
            c.name = newName;
            found = true;
            break;
        }
    }
    if (!found) throw std::runtime_error("Column '" + oldName + "' not found");
    saveSchema();
}