#include "pager.h"
#include <cstring>
#include <stdexcept>

Pager::Pager(const std::string& filename) {
    file_ = fopen(filename.c_str(), "r+b");
    if (file_ == nullptr) {
        // File doesn't exist yet -- create it.
        file_ = fopen(filename.c_str(), "w+b");
        if (file_ == nullptr) {
            throw std::runtime_error("Unable to open database file: " + filename);
        }
    }

    fseek(file_, 0, SEEK_END);
    long fileSizeBytes = ftell(file_);
    fseek(file_, 0, SEEK_SET);

    if (fileSizeBytes % PAGE_SIZE != 0) {
        throw std::runtime_error("Corrupt database file: not a whole number of pages");
    }

    fileLengthPages_ = static_cast<uint32_t>(fileSizeBytes / PAGE_SIZE);
    numPages_ = fileLengthPages_;
    pages_.resize(MAX_PAGES, nullptr);
}

Pager::~Pager() {
    flushAll();
    if (file_) fclose(file_);
    for (char* p : pages_) {
        delete[] p;
    }
}

char* Pager::getPage(uint32_t pageNum) {
    if (pageNum >= MAX_PAGES) {
        throw std::runtime_error("Page number out of bounds");
    }

    if (pages_[pageNum] == nullptr) {
        char* page = new char[PAGE_SIZE];
        memset(page, 0, PAGE_SIZE);

        if (pageNum < fileLengthPages_) {
            fseek(file_, static_cast<long>(pageNum) * PAGE_SIZE, SEEK_SET);
            size_t bytesRead = fread(page, 1, PAGE_SIZE, file_);
            (void)bytesRead; // short reads at EOF are fine; rest stays zeroed
        }

        pages_[pageNum] = page;
        if (pageNum >= numPages_) {
            numPages_ = pageNum + 1;
        }
    }

    return pages_[pageNum];
}

uint32_t Pager::allocatePage() {
    uint32_t pageNum = numPages_;
    getPage(pageNum); // forces allocation of the in-memory buffer
    numPages_ = pageNum + 1;
    return pageNum;
}

void Pager::flushPage(uint32_t pageNum) {
    if (pages_[pageNum] == nullptr) return;
    fseek(file_, static_cast<long>(pageNum) * PAGE_SIZE, SEEK_SET);
    fwrite(pages_[pageNum], 1, PAGE_SIZE, file_);
}

void Pager::flushAll() {
    if (!file_) return;
    for (uint32_t i = 0; i < numPages_; i++) {
        flushPage(i);
    }
    fflush(file_);
}
