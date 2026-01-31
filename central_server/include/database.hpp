#pragma once

#include <iostream>
#include <pqxx/pqxx>

#include "json.hpp"

using json = nlohmann::json;

namespace DB {

typedef struct Transfer_Payload {
    std::string id;
    std::string sender_ip;
    uint32_t sender_port;
    std::string protocol;
    std::string file_name;
    size_t file_size;
    std::string file_hash;
    std::string token;
} TRANSFERS;

// Full row representation returned from DB (includes nullable columns)
struct TransferRow {
    std::string id;
    std::string sender_ip;
    uint32_t sender_port{};
    std::string protocol;
    std::string file_name;  // empty if NULL
    long file_size{};       // 0 if NULL
    std::string file_hash;  // empty if NULL
    std::string token;
    bool claimed{};          // false if NULL
    std::string created_at;  // empty if NULL
};

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

void reconnect() {
    std::cerr << "Attempting to reconnect to database...\n";
    try {
        connection.reset(); // Close old connection
        connection = std::make_unique<pqxx::connection>(get_env("DATABASE_URL"));
        if (connection->is_open()) {
            std::cout << "Reconnected to Database Successfully!\n";
        } else {
             std::cerr << "Reconnection failed: connection not open.\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "Reconnection failed: " << e.what() << "\n";
        // Convert to broken_connection so retry loop continues/fails appropriately if needed, 
        // or just let the next usage fail.
    }
}

template<typename Func>
auto retry_operation(Func action) {
    int retries = 3;
    while (true) {
        try {
            return action();
        } catch (const pqxx::broken_connection& e) {
            std::cerr << "Database connection broken: " << e.what() << "\n";
            if (--retries < 0) throw; // Rethrow if out of retries
            reconnect();
            std::this_thread::sleep_for(std::chrono::seconds(1)); // Wait a bit
        } catch (const std::exception& e) {
             // Other DB errors might not be recoverable by reconnecting, but let's be safe?
             // Usually retry only on connection issues.
             throw; 
        }
    }
}

void create_transfers_table() {
    pqxx::work txn(conn());

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS transfers (
            id TEXT PRIMARY KEY,
            sender_ip TEXT NOT NULL,
            sender_port INTEGER NOT NULL,
            protocol TEXT NOT NULL,
            file_name TEXT,
            file_size BIGINT,
            file_hash TEXT,
            token TEXT NOT NULL,
            claimed BOOLEAN DEFAULT FALSE,
            created_at TIMESTAMP DEFAULT now()
        );
    )");

    txn.commit();

    std::cout << " Table 'transfers' created\n";
}

void create_tables() { create_transfers_table(); }

// Convert pqxx::row into a TransferRow with safe defaults for NULLs
TransferRow to_transfer_row(const pqxx::row& row) {
    TransferRow t{};
    t.id = row["id"].as<std::string>();
    t.sender_ip = row["sender_ip"].as<std::string>();
    t.sender_port = row["sender_port"].as<uint32_t>();
    t.protocol = row["protocol"].as<std::string>();
    t.file_name = row["file_name"].is_null() ? "" : row["file_name"].as<std::string>();
    t.file_size = row["file_size"].is_null() ? 0 : row["file_size"].as<long>();
    t.file_hash = row["file_hash"].is_null() ? "" : row["file_hash"].as<std::string>();
    t.token = row["token"].as<std::string>();
    t.claimed = row["claimed"].is_null() ? false : row["claimed"].as<bool>();
    t.created_at = row["created_at"].is_null() ? "" : row["created_at"].c_str();
    return t;
}

inline json transfer_row_to_json(const TransferRow& t) {
    return json{{"id", t.id},
                {"sender_ip", t.sender_ip},
                {"sender_port", t.sender_port},
                {"protocol", t.protocol},
                {"file_name", t.file_name},
                {"file_size", t.file_size},
                {"file_hash", t.file_hash},
                {"token", t.token},
                {"claimed", t.claimed},
                {"created_at", t.created_at}};
}

inline TRANSFERS transfer_row_from_json(const json& data) {
    try {
        TRANSFERS t;
        t.id = data["id"];
        t.sender_ip = data["sender_ip"];
        t.sender_port = data["sender_port"];
        t.protocol = data["protocol"];
        t.file_name = data["file_name"];
        t.file_size = data["file_size"];
        t.file_hash = data["file_hash"];
        t.token = data["token"];
        return t;
    } catch (std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        throw std::runtime_error(e.what());
    }
}

void print_transfer(const TransferRow& t) {
    std::cout << "id: " << t.id << "\n";
    std::cout << "sender_ip: " << t.sender_ip << "\n";
    std::cout << "sender_port: " << t.sender_port << "\n";
    std::cout << "protocol: " << t.protocol << "\n";
    std::cout << "file_name: " << t.file_name << "\n";
    std::cout << "file_size: " << t.file_size << "\n";
    std::cout << "file_hash: " << t.file_hash << "\n";
    std::cout << "token: " << t.token << "\n";
    std::cout << "claimed: " << t.claimed << "\n";
    std::cout << "created_at: " << t.created_at << "\n";
}

bool lookup_transfer(const std::string_view secret) {
    return retry_operation([&]() {
        pqxx::read_transaction txn(conn());
        pqxx::result r = txn.exec_params("SELECT * FROM transfers WHERE id = $1", std::string(secret));
        return !r.empty();
    });
}

void mark_transfer_claimed(const std::string_view secret) {
    retry_operation([&]() {
        pqxx::work txn(conn());
        txn.exec_params("UPDATE transfers SET claimed = true WHERE id = $1", std::string(secret));
        txn.commit();
        return true; // dummy return
    });
}

TransferRow get_transfers_metadata(const std::string_view secret) {
    return retry_operation([&]() {
        pqxx::work txn(conn()); // Use work for atomic read + update if possible, or keep separate
        // Note: The original had read_transaction then mark_transfer_claimed which is a separate txn.
        // Let's keep logic similar but robust.
        
        pqxx::result r = txn.exec_params("SELECT * FROM transfers WHERE id = $1", std::string(secret));
        if (r.empty()) throw std::runtime_error("Transfer not found");
        
        TransferRow data = to_transfer_row(r[0]);
        // Ideally we commit this read if we were locking, but we are just reading.
        txn.commit(); // Not strictly needed for read but fine.

        mark_transfer_claimed(secret); // This has its own retry logic if we wrap it, or we wrap the whole thing.
        // mark_transfer_claimed already creates a transaction.
        
        data.claimed = true;
        return data;
    });
}

bool register_transfers(const json data) {
    return retry_operation([&]() {
        pqxx::work txn(conn());
        TRANSFERS payload = transfer_row_from_json(data);
        txn.exec_params(
            R"(
            INSERT INTO transfers (
                id,
                sender_ip,
                sender_port,
                protocol,
                file_name,
                file_size,
                file_hash,
                token
            ) VALUES ($1, $2, $3, $4, $5, $6, $7, $8)
            ON CONFLICT (id) DO UPDATE SET
                sender_ip = EXCLUDED.sender_ip,
                sender_port = EXCLUDED.sender_port,
                protocol = EXCLUDED.protocol,
                file_name = EXCLUDED.file_name,
                file_size = EXCLUDED.file_size,
                file_hash = EXCLUDED.file_hash,
                token = EXCLUDED.token
        )",
            payload.id, payload.sender_ip, payload.sender_port, payload.protocol, payload.file_name, payload.file_size,
            payload.file_hash, payload.token);

        txn.commit();
        return true;
    });
}
}  // namespace DB