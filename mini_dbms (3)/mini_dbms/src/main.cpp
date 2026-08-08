#include <iostream>
#include <string>
#include "database.h"
#include "sql.h"

static void printBanner(const std::string& dbFile) {
    std::cout << "mini-dbms -- a tiny disk-backed database engine (C++)\n"
              << "database file: " << dbFile << "\n"
              << "type SQL statements ending without ';' is fine, one per line.\n"
              << "meta-commands: .exit, .help\n"
              << "example:\n"
              << "  CREATE TABLE users (id INT, name VARCHAR(32), email VARCHAR(64))\n"
              << "  INSERT INTO users VALUES (1, 'alice', 'alice@example.com')\n"
              << "  SELECT * FROM users WHERE id = 1\n"
              << "  DROP TABLE users\n"
              << "  SHOW TABLES\n\n";
}

int main(int argc, char** argv) {
    std::string dbFile = (argc > 1) ? argv[1] : "mini.db";

    Database db(dbFile);

    printBanner(dbFile);
    auto tables = db.listTables();
    if (!tables.empty()) {
        std::cout << "Loaded existing tables: ";
        for (size_t i = 0; i < tables.size(); i++) {
            std::cout << tables[i];
            if (i + 1 < tables.size()) std::cout << ", ";
        }
        std::cout << "\n\n";
    }

    std::string line;
    while (true) {
        std::cout << "mini-dbms> ";
        if (!std::getline(std::cin, line)) break;

        std::string trimmed = line;
        size_t a = trimmed.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        trimmed = trimmed.substr(a);

        if (trimmed == ".exit" || trimmed == ".quit") break;
        if (trimmed == ".help") {
            printBanner(dbFile);
            continue;
        }

        try {
            std::string result = Engine::execute(db, line);
            if (!result.empty()) std::cout << result << "\n";
        } catch (const std::exception& e) {
            std::cout << "Error: " << e.what() << "\n";
        }
    }

    std::cout << "bye.\n";
    return 0;
}