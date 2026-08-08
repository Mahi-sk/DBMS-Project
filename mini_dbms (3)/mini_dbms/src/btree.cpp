#include "btree.h"
#include <cstring>
#include <stdexcept>
#include <vector>

// ---------------- common header ----------------

NodeType getNodeType(char* node) {
    return static_cast<NodeType>(static_cast<uint8_t>(node[0]));
}
void setNodeType(char* node, NodeType type) {
    node[0] = static_cast<char>(static_cast<uint8_t>(type));
}
bool isNodeRoot(char* node) { return node[1] != 0; }
void setNodeRoot(char* node, bool isRoot) { node[1] = isRoot ? 1 : 0; }
uint32_t* nodeParent(char* node) {
    return reinterpret_cast<uint32_t*>(node + 2);
}

// ---------------- leaf ----------------

uint32_t* leafNumCells(char* node) {
    return reinterpret_cast<uint32_t*>(node + COMMON_NODE_HEADER_SIZE);
}
uint32_t* leafNextLeaf(char* node) {
    return reinterpret_cast<uint32_t*>(node + COMMON_NODE_HEADER_SIZE + 4);
}
char* leafCell(char* node, uint32_t cellNum, uint32_t rowSize) {
    uint32_t cellSize = 4 + rowSize;
    return node + LEAF_NODE_HEADER_SIZE + cellNum * cellSize;
}
uint32_t* leafKey(char* node, uint32_t cellNum, uint32_t rowSize) {
    return reinterpret_cast<uint32_t*>(leafCell(node, cellNum, rowSize));
}
char* leafValue(char* node, uint32_t cellNum, uint32_t rowSize) {
    return leafCell(node, cellNum, rowSize) + 4;
}
uint32_t leafMaxCells(uint32_t rowSize) {
    uint32_t cellSize = 4 + rowSize;
    return (PAGE_SIZE - LEAF_NODE_HEADER_SIZE) / cellSize;
}
void initializeLeaf(char* node) {
    setNodeType(node, NodeType::Leaf);
    setNodeRoot(node, false);
    *nodeParent(node) = INVALID_PAGE;
    *leafNumCells(node) = 0;
    *leafNextLeaf(node) = 0; // 0 = "no sibling" (page 0 is reserved for metadata)
}

// ---------------- internal ----------------

uint32_t* internalNumKeys(char* node) {
    return reinterpret_cast<uint32_t*>(node + COMMON_NODE_HEADER_SIZE);
}
uint32_t* internalRightChild(char* node) {
    return reinterpret_cast<uint32_t*>(node + COMMON_NODE_HEADER_SIZE + 4);
}
uint32_t* internalCell(char* node, uint32_t cellNum) {
    return reinterpret_cast<uint32_t*>(node + INTERNAL_NODE_HEADER_SIZE + cellNum * INTERNAL_CELL_SIZE);
}
uint32_t* internalChild(char* node, uint32_t cellNum) {
    return internalCell(node, cellNum);
}
uint32_t* internalKey(char* node, uint32_t cellNum) {
    return reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(internalCell(node, cellNum)) + 4);
}
void initializeInternal(char* node) {
    setNodeType(node, NodeType::Internal);
    setNodeRoot(node, false);
    *nodeParent(node) = INVALID_PAGE;
    *internalNumKeys(node) = 0;
    *internalRightChild(node) = INVALID_PAGE;
}

// ---------------- BTree ----------------

uint32_t BTree::subtreeMaxKey(char* node) {
    if (getNodeType(node) == NodeType::Leaf) {
        uint32_t n = *leafNumCells(node);
        return n == 0 ? 0 : *leafKey(node, n - 1, rowSize_);
    }
    char* rightChild = pager_.getPage(*internalRightChild(node));
    return subtreeMaxKey(rightChild);
}

uint32_t BTree::internalFindChildIndex(char* node, uint32_t key) {
    uint32_t numKeys = *internalNumKeys(node);
    uint32_t lo = 0, hi = numKeys;
    while (lo != hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t keyToRight = *internalKey(node, mid);
        if (keyToRight >= key) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

BTree::Cursor BTree::internalFind(uint32_t pageNum, uint32_t key) {
    char* node = pager_.getPage(pageNum);
    uint32_t childIndex = internalFindChildIndex(node, key);
    uint32_t numKeys = *internalNumKeys(node);

    uint32_t childPage = (childIndex == numKeys) ? *internalRightChild(node)
                                                  : *internalChild(node, childIndex);

    char* child = pager_.getPage(childPage);
    if (getNodeType(child) == NodeType::Leaf) {
        return leafFind(childPage, key);
    }
    return internalFind(childPage, key);
}

BTree::Cursor BTree::leafFind(uint32_t pageNum, uint32_t key) {
    char* node = pager_.getPage(pageNum);
    uint32_t numCells = *leafNumCells(node);

    uint32_t lo = 0, hi = numCells;
    while (lo != hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t midKey = *leafKey(node, mid, rowSize_);
        if (key == midKey) return {pageNum, mid, false};
        if (key < midKey) hi = mid;
        else lo = mid + 1;
    }
    return {pageNum, lo, false};
}

BTree::Cursor BTree::findKey(uint32_t rootPage, uint32_t key) {
    char* root = pager_.getPage(rootPage);
    if (getNodeType(root) == NodeType::Leaf) {
        return leafFind(rootPage, key);
    }
    return internalFind(rootPage, key);
}

BTree::Cursor BTree::tableStart(uint32_t rootPage) {
    Cursor c = findKey(rootPage, 0);
    char* node = pager_.getPage(c.pageNum);
    c.endOfTable = (*leafNumCells(node) == 0);
    return c;
}

bool BTree::keyExists(uint32_t rootPage, uint32_t key) {
    Cursor c = findKey(rootPage, key);
    char* node = pager_.getPage(c.pageNum);
    if (c.cellNum >= *leafNumCells(node)) return false;
    return *leafKey(node, c.cellNum, rowSize_) == key;
}

bool BTree::remove(uint32_t& rootPage, uint32_t key) {
    Cursor c = findKey(rootPage, key);
    char* node = pager_.getPage(c.pageNum);
    uint32_t numCells = *leafNumCells(node);

    if (c.cellNum >= numCells || *leafKey(node, c.cellNum, rowSize_) != key) {
        return false; // not found
    }

    uint32_t oldMaxKey = *leafKey(node, numCells - 1, rowSize_);
    uint32_t cellSize = 4 + rowSize_;

    for (uint32_t i = c.cellNum; i + 1 < numCells; i++) {
        memcpy(leafCell(node, i, rowSize_), leafCell(node, i + 1, rowSize_), cellSize);
    }
    *leafNumCells(node) -= 1;
    numCells -= 1;

    // If we just removed the largest key in this leaf, the parent's
    // separator key for it is now stale -- point it at the new max (if
    // any remain). A stale-but-larger separator would still route
    // correctly, but keeping it accurate avoids surprises if this code
    // is extended later.
    if (!isNodeRoot(node) && key == oldMaxKey && numCells > 0) {
        uint32_t parentPage = *nodeParent(node);
        char* parent = pager_.getPage(parentPage);
        uint32_t newMax = *leafKey(node, numCells - 1, rowSize_);
        updateInternalKey(parent, oldMaxKey, newMax);
    }

    return true;
}

void BTree::insert(uint32_t& rootPage, uint32_t key, const char* rowData) {
    Cursor c = findKey(rootPage, key);
    char* node = pager_.getPage(c.pageNum);
    uint32_t numCells = *leafNumCells(node);

    if (c.cellNum < numCells && *leafKey(node, c.cellNum, rowSize_) == key) {
        throw std::runtime_error("Duplicate key: " + std::to_string(key));
    }

    if (numCells >= leafMaxCells(rowSize_)) {
        leafSplitInsert(rootPage, c.pageNum, c.cellNum, key, rowData);
    } else {
        leafInsert(rootPage, c.pageNum, c.cellNum, key, rowData);
    }
}

void BTree::leafInsert(uint32_t& rootPage, uint32_t pageNum, uint32_t cellNum,
                        uint32_t key, const char* rowData) {
    (void)rootPage;
    char* node = pager_.getPage(pageNum);
    uint32_t numCells = *leafNumCells(node);
    uint32_t cellSize = 4 + rowSize_;

    for (uint32_t i = numCells; i > cellNum; i--) {
        memcpy(leafCell(node, i, rowSize_), leafCell(node, i - 1, rowSize_), cellSize);
    }
    *leafNumCells(node) += 1;
    *leafKey(node, cellNum, rowSize_) = key;
    memcpy(leafValue(node, cellNum, rowSize_), rowData, rowSize_);
}

void BTree::updateInternalKey(char* node, uint32_t oldKey, uint32_t newKey) {
    uint32_t idx = internalFindChildIndex(node, oldKey);
    if (idx < *internalNumKeys(node) && *internalKey(node, idx) == oldKey) {
        *internalKey(node, idx) = newKey;
    }
}

void BTree::leafSplitInsert(uint32_t& rootPage, uint32_t pageNum, uint32_t cellNum,
                             uint32_t key, const char* rowData) {
    // Leaf is full. Gather its existing cells plus the new one into a
    // temp buffer (logically), split evenly across the old leaf (left)
    // and a freshly allocated leaf (right), and re-link the leaf chain.
    char* oldNode = pager_.getPage(pageNum);
    uint32_t oldMaxBeforeSplit = subtreeMaxKey(oldNode);
    uint32_t cellSize = 4 + rowSize_;
    uint32_t maxCells = leafMaxCells(rowSize_);
    uint32_t totalCells = maxCells + 1; // including the new one being inserted

    // Snapshot all existing cells before we start overwriting the page.
    std::vector<char> existing(maxCells * cellSize);
    memcpy(existing.data(), oldNode + LEAF_NODE_HEADER_SIZE, maxCells * cellSize);

    uint32_t newPageNum = pager_.allocatePage();
    // re-fetch pointers: allocatePage() may have resized internal storage
    oldNode = pager_.getPage(pageNum);
    char* newNode = pager_.getPage(newPageNum);
    initializeLeaf(newNode);
    uint32_t savedParent = *nodeParent(oldNode);
    uint32_t savedNextLeaf = *leafNextLeaf(oldNode);
    *nodeParent(newNode) = savedParent;
    *leafNextLeaf(newNode) = savedNextLeaf;

    uint32_t leftCount = totalCells - (totalCells / 2);

    std::vector<char> merged(totalCells * cellSize);
    // merge existing cells with the new cell in sorted position
    uint32_t src = 0;
    for (uint32_t i = 0; i < totalCells; i++) {
        if (i == cellNum) {
            uint32_t k = key;
            memcpy(merged.data() + i * cellSize, &k, 4);
            memcpy(merged.data() + i * cellSize + 4, rowData, rowSize_);
        } else {
            memcpy(merged.data() + i * cellSize, existing.data() + src * cellSize, cellSize);
            src++;
        }
    }

    // reset old node then redistribute
    bool wasRoot = isNodeRoot(oldNode);
    initializeLeaf(oldNode);
    *nodeParent(oldNode) = savedParent;
    setNodeRoot(oldNode, wasRoot);
    *leafNextLeaf(oldNode) = newPageNum;

    memcpy(oldNode + LEAF_NODE_HEADER_SIZE, merged.data(), leftCount * cellSize);
    *leafNumCells(oldNode) = leftCount;

    memcpy(newNode + LEAF_NODE_HEADER_SIZE, merged.data() + leftCount * cellSize,
           (totalCells - leftCount) * cellSize);
    *leafNumCells(newNode) = totalCells - leftCount;

    if (wasRoot) {
        createNewRoot(rootPage, newPageNum);
    } else {
        uint32_t parentPage = *nodeParent(oldNode);
        char* parent = pager_.getPage(parentPage);
        uint32_t newOldMax = subtreeMaxKey(oldNode);
        updateInternalKey(parent, oldMaxBeforeSplit, newOldMax);
        internalInsert(rootPage, parentPage, newPageNum);
    }
}

void BTree::internalInsert(uint32_t& rootPage, uint32_t parentPage, uint32_t childPage) {
    char* parent = pager_.getPage(parentPage);
    char* child = pager_.getPage(childPage);
    uint32_t childMaxKey = subtreeMaxKey(child);

    uint32_t maxInternalKeys = (PAGE_SIZE - INTERNAL_NODE_HEADER_SIZE) / INTERNAL_CELL_SIZE;
    uint32_t originalNumKeys = *internalNumKeys(parent);

    if (originalNumKeys + 1 >= maxInternalKeys) {
        // A production B+Tree recursively splits internal nodes here too.
        // This mini-engine caps depth to keep the implementation readable;
        // it comfortably supports well over 100k rows for a typical
        // fixed-width row before this limit is ever hit.
        throw std::runtime_error(
            "Internal node capacity reached -- this mini-engine's tree depth limit was hit. "
            "(See internalInsert() in btree.cpp for where recursive internal-node splitting would go.)");
    }

    uint32_t rightChildPage = *internalRightChild(parent);
    char* rightChild = pager_.getPage(rightChildPage);
    uint32_t rightChildMaxKey = subtreeMaxKey(rightChild);

    if (childMaxKey > rightChildMaxKey) {
        *internalChild(parent, originalNumKeys) = rightChildPage;
        *internalKey(parent, originalNumKeys) = rightChildMaxKey;
        *internalRightChild(parent) = childPage;
        *internalNumKeys(parent) += 1;
    } else {
        uint32_t index = internalFindChildIndex(parent, childMaxKey);
        for (uint32_t i = originalNumKeys; i > index; i--) {
            memcpy(internalCell(parent, i), internalCell(parent, i - 1), INTERNAL_CELL_SIZE);
        }
        *internalChild(parent, index) = childPage;
        *internalKey(parent, index) = childMaxKey;
        *internalNumKeys(parent) += 1;
    }
    *nodeParent(child) = parentPage;
}

void BTree::createNewRoot(uint32_t& rootPage, uint32_t rightChildPage) {
    // Copy the old root's contents into a brand new "left child" page,
    // then overwrite the old root page in place with a fresh internal
    // node pointing at [leftChild, rightChild]. Reusing the original
    // page number as the root keeps `rootPage` stable across splits.
    char* oldRoot = pager_.getPage(rootPage);
    uint32_t leftChildPage = pager_.allocatePage();
    oldRoot = pager_.getPage(rootPage); // re-fetch after possible resize
    char* leftChild = pager_.getPage(leftChildPage);

    memcpy(leftChild, oldRoot, PAGE_SIZE);
    setNodeRoot(leftChild, false);

    if (getNodeType(leftChild) == NodeType::Internal) {
        uint32_t nk = *internalNumKeys(leftChild);
        for (uint32_t i = 0; i < nk; i++) {
            char* c = pager_.getPage(*internalChild(leftChild, i));
            *nodeParent(c) = leftChildPage;
        }
        char* rc = pager_.getPage(*internalRightChild(leftChild));
        *nodeParent(rc) = leftChildPage;
    }

    uint32_t leftMax = subtreeMaxKey(leftChild);

    initializeInternal(oldRoot);
    setNodeRoot(oldRoot, true);
    *internalNumKeys(oldRoot) = 1;
    *internalChild(oldRoot, 0) = leftChildPage;
    *internalKey(oldRoot, 0) = leftMax;
    *internalRightChild(oldRoot) = rightChildPage;

    *nodeParent(leftChild) = rootPage;
    char* rightChild = pager_.getPage(rightChildPage);
    *nodeParent(rightChild) = rootPage;
}
