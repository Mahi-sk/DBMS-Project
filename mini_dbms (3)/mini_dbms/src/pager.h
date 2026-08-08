#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// A DBMS stores data on disk in fixed-size pages. The Pager is the layer
// that hides the file system from the rest of the engine: everyone else
// just asks for "page N" and gets back a 4KB buffer they can read/write.
// Pages are cached in memory once loaded and flushed back on close.

constexpr uint32_t PAGE_SIZE = 4096;
constexpr uint32_t MAX_PAGES = 100000;

class Pager {
public:
    explicit Pager(const std::string& filename);
    ~Pager();

    // Returns a pointer to an in-memory buffer for the given page.
    // Loads it from disk on first access; subsequent calls are cached.
    char* getPage(uint32_t pageNum);

    // Allocates a brand-new page at the end of the file and returns its
    // page number. Content is zero-initialized.
    uint32_t allocatePage();

    uint32_t numPages() const { return numPages_; }

    void flushAll();

private:
    void flushPage(uint32_t pageNum);

    FILE* file_;
    uint32_t fileLengthPages_;
    uint32_t numPages_;
    std::vector<char*> pages_; // lazily-populated cache, indexed by page num
};
