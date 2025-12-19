#pragma once

#include <iostream>
#include <pqxx/pqxx>

namespace DB {

static std::unique_ptr<pqxx::connection> connection;

const char* get_env(const char* key) {
    const char* value = std::getenv(key);
    if (!value) {
        throw std::runtime_error(std::string("Missing environment variable: ") + key);
    }
    return value;
}

void init() {
    connection = std::make_unique<pqxx::connection>(get_env("DATABASE_URL"));

    if (!connection->is_open()) {
        throw std::runtime_error("Failed to open DB connection");
    }
    std::cout << "Connected to Database Successfully!\n";
}

pqxx::connection& conn() {
    if (!connection) {
        throw std::runtime_error("DB not initialized");
    }
    return *connection;
}

void list_files() {
    pqxx::read_transaction txn(DB::conn());

    pqxx::result r = txn.exec("SELECT id, name, size FROM files");

    for (const auto& row : r) {
        int id = row["id"].as<int>();
        std::string name = row["name"].as<std::string>();
        long size = row["size"].as<long>();

        std::cout << id << " " << name << " " << size << "\n";
    }
}
}  // namespace DB