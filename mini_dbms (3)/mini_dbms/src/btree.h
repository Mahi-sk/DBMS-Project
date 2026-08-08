#pragma once
#include <cstdint>
#include "pager.h"

// ---------------------------------------------------------------------
// B+Tree node layout
//
// Every page is either a LEAF node (holds actual rows, keyed by an
// integer primary key) or an INTERNAL node (holds pointers to child
// pages, used for routing during search). This is the same design
// SQLite uses for its table b-trees: leaves are linked together
// (next_leaf) so a full table scan is a simple linked-list walk, and
// point/range lookups walk down from the root comparing keys.
//
//   Common header (both node types):
//     [0]      node_type   (1 byte)
//     [1]      is_root     (1 byte)
//     [2..5]   parent ptr  (4 bytes)
//
//   Leaf node (after common header):
//     [6..9]   num_cells   (4 bytes)
//     [10..13] next_leaf   (4 bytes, 0 = no sibling / this is rightmost)
//     cells: [key(4)] [row bytes (variable, fixed per-table)] ...
//
//   Internal node (after common header):
//     [6..9]   num_keys      (4 bytes)
//     [10..13] right_child   (4 bytes)
//     cells: [child_ptr(4)] [key(4)] ...   (num_keys of these)
// ---------------------------------------------------------------------

enum class NodeType : uint8_t { Leaf = 0, Internal = 1 };

constexpr uint32_t COMMON_NODE_HEADER_SIZE = 6;
constexpr uint32_t LEAF_NODE_HEADER_SIZE = COMMON_NODE_HEADER_SIZE + 8;
constexpr uint32_t INTERNAL_NODE_HEADER_SIZE = COMMON_NODE_HEADER_SIZE + 8;
constexpr uint32_t INTERNAL_CELL_SIZE = 8; // child ptr (4) + key (4)
constexpr uint32_t INVALID_PAGE = 0xFFFFFFFF;

// --- generic header accessors ---
NodeType getNodeType(char* node);
void setNodeType(char* node, NodeType type);
bool isNodeRoot(char* node);
void setNodeRoot(char* node, bool isRoot);
uint32_t* nodeParent(char* node);

// --- leaf accessors ---
uint32_t* leafNumCells(char* node);
uint32_t* leafNextLeaf(char* node);

char* leafCell(char* node, uint32_t cellNum, uint32_t rowSize);
uint32_t* leafKey(char* node, uint32_t cellNum, uint32_t rowSize);
char* leafValue(char* node, uint32_t cellNum, uint32_t rowSize);
uint32_t leafMaxCells(uint32_t rowSize);

// --- internal accessors ---
uint32_t* internalNumKeys(char* node);
uint32_t* internalRightChild(char* node);
uint32_t* internalCell(char* node, uint32_t cellNum);
uint32_t* internalChild(char* node, uint32_t cellNum);
uint32_t* internalKey(char* node, uint32_t cellNum);

void initializeLeaf(char* node);
void initializeInternal(char* node);

// ---------------------------------------------------------------------
// BTree: the operations that work across nodes (search / insert / split).
// It knows the row size for the table it belongs to, since leaf cells
// are sized based on the table's schema.
// ---------------------------------------------------------------------
class BTree {
public:
    BTree(Pager& pager, uint32_t rowSize) : pager_(pager), rowSize_(rowSize) {}

    // Returns the page number and cell index where `key` is or should be
    // inserted, starting the search at `pageNum`.
    struct Cursor {
        uint32_t pageNum;
        uint32_t cellNum;
        bool endOfTable;
    };

    Cursor findKey(uint32_t rootPage, uint32_t key);
    Cursor tableStart(uint32_t rootPage);

    // Inserts (key, rowData) into the tree rooted at *rootPage.
    // May change *rootPage if the root splits.
    void insert(uint32_t& rootPage, uint32_t key, const char* rowData);

    bool keyExists(uint32_t rootPage, uint32_t key);

    // Removes `key` if present. Returns false if the key wasn't found.
    // NOTE: this only removes the cell from its leaf -- it does not merge
    // underfull leaves with siblings or shrink internal nodes. That keeps
    // the implementation simple at the cost of not reclaiming space or
    // maintaining the "half full" B+Tree invariant after heavy deletion.
    // Lookups/inserts/scans remain correct either way.
    bool remove(uint32_t& rootPage, uint32_t key);

private:
    Pager& pager_;
    uint32_t rowSize_;

    Cursor leafFind(uint32_t pageNum, uint32_t key);
    Cursor internalFind(uint32_t pageNum, uint32_t key);

    void leafInsert(uint32_t& rootPage, uint32_t pageNum, uint32_t cellNum,
                     uint32_t key, const char* rowData);
    void leafSplitInsert(uint32_t& rootPage, uint32_t pageNum, uint32_t cellNum,
                          uint32_t key, const char* rowData);
    void internalInsert(uint32_t& rootPage, uint32_t parentPage, uint32_t childPage);
    void createNewRoot(uint32_t& rootPage, uint32_t rightChildPage);
    uint32_t internalFindChildIndex(char* node, uint32_t key);
    void updateInternalKey(char* node, uint32_t oldKey, uint32_t newKey);

    // Max key stored under a subtree rooted at `node` (leaf: last key in
    // the leaf; internal: recurse down the rightmost child).
    uint32_t subtreeMaxKey(char* node);
};
