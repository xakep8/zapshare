#define CPPHTTPLIB_OPENSSL_SUPPORT

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <map>
#include <vector>
#include <memory>

#include "database.hpp"
#include "httplib.h"
#include "json.hpp"

using json = nlohmann::json;

// SSE signal management
struct SignalManager {
    std::mutex mutex;
    std::map<std::string, json> signal_store;
    std::map<std::string, std::vector<std::shared_ptr<std::condition_variable>>> waiters;
    
    void set_signal(const std::string& id, const json& data) {
        std::lock_guard<std::mutex> lock(mutex);
        signal_store[id] = data;
        
        // Notify all waiting clients for this id
        if (waiters.count(id)) {
            for (auto& cv : waiters[id]) {
                cv->notify_all();
            }
            waiters[id].clear();
        }
    }
    
    json get_signal(const std::string& id) {
        std::lock_guard<std::mutex> lock(mutex);
        if (signal_store.count(id)) {
            return signal_store[id];
        }
        return nullptr;
    }
    
    std::shared_ptr<std::condition_variable> register_waiter(const std::string& id) {
        std::lock_guard<std::mutex> lock(mutex);
        auto cv = std::make_shared<std::condition_variable>();
        waiters[id].push_back(cv);
        return cv;
    }
};

static SignalManager signal_manager;


int main() {
    DB::init();
    DB::create_tables();

    httplib::Server svr;
    // httplib::SSLServer svr;

    // Request logger
    svr.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        std::cout << req.method << " " << req.path << " -> " << res.status << std::endl;
    });

    // Error logger
    svr.set_error_logger([](const httplib::Error& err, const httplib::Request* req) {
        std::cerr << httplib::to_string(err) << " while processing request";
        if (req) {
            std::cerr << ", client: " << req->get_header_value("X-Forwarded-For") << ", request: '" << req->method
                      << " " << req->path << " " << req->version << "'"
                      << ", host: " << req->get_header_value("Host");
        }
        std::cerr << std::endl;
    });

    // Error handler
    svr.set_error_handler([](const auto& req, auto& res) {
        auto fmt = "Error Status: %d";
        char buf[BUFSIZ];
        snprintf(buf, sizeof(buf), fmt, res.status);
        res.set_content(buf, "text/plain");
    });

    // Exception Handler
    svr.set_exception_handler([](const auto& req, auto& res, std::exception_ptr ep) {
        auto fmt = "Error 500: %s";
        char buf[BUFSIZ];
        try {
            std::rethrow_exception(ep);
        } catch (std::exception& e) {
            snprintf(buf, sizeof(buf), fmt, e.what());
        } catch (...) {  // See the following NOTE
            snprintf(buf, sizeof(buf), fmt, "Unknown Exception");
        }
        res.set_content(buf, "text/plain");
        res.status = httplib::StatusCode::InternalServerError_500;
    });

    svr.Get(R"(/lookup/([A-Za-z0-9\-]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string secret = req.matches[1];
        std::cout << secret << std::endl;
        std::string result = DB::lookup_transfer(secret) ? "True" : "False";
        res.set_content(result, "text/plain");
    });

    svr.Get(R"(/getfile/([A-Za-z0-9\-]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string secret = req.matches[1];
        try {
            DB::TransferRow row = DB::get_transfers_metadata(secret);
            std::string data = DB::transfer_row_to_json(row).dump();
            res.set_content(data, "application/json");
            res.status = httplib::StatusCode::OK_200;
        } catch (std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
        }
    });

    svr.Post("/register", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json data = json::parse(req.body);
            DB::register_transfers(data);
            res.status = httplib::StatusCode::Created_201;
        } catch (std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
        }
    });

    // Simple in-memory storage for signaling (id -> json)
    // Note: In a production environment, this should probably be in the database or Redis
    // Now using SSE for real-time notification instead of polling

    svr.Post(R"(/signal/([A-Za-z0-9\-]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.matches[1];
        try {
            json data = json::parse(req.body);
            signal_manager.set_signal(id, data);
            std::cout << "Signal received for " << id << ": " << data.dump() << std::endl;
            res.status = httplib::StatusCode::OK_200;
        } catch (std::exception& e) {
            std::cerr << "Error parsing signal: " << e.what() << "\n";
            res.status = httplib::StatusCode::BadRequest_400;
        }
    });

    svr.Get(R"(/signal/([A-Za-z0-9\-]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.matches[1];
        
        // Set SSE content type and headers
        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        res.set_header("Access-Control-Allow-Origin", "*");
        res.status = httplib::StatusCode::OK_200;
        
        // Check if signal already exists (fast path, no wait needed)
        json existing_signal = signal_manager.get_signal(id);
        if (existing_signal != nullptr) {
            std::string sse_data = "data: " + existing_signal.dump() + "\n\n";
            res.set_content(sse_data, "text/event-stream");
            return;
        }
        
        // Register waiter and wait for signal with timeout
        auto cv = signal_manager.register_waiter(id);
        std::unique_lock<std::mutex> lock(signal_manager.mutex);
        
        // Wait up to 15 minutes for signal
        auto timeout = std::chrono::seconds(900);
        bool signaled = cv->wait_for(lock, timeout, [&id]() {
            return signal_manager.get_signal(id) != nullptr;
        });
        
        if (signaled) {
            json signal_data = signal_manager.get_signal(id);
            std::string sse_data = "data: " + signal_data.dump() + "\n\n";
            res.set_content(sse_data, "text/event-stream");
        } else {
            // Timeout - return 408 Request Timeout
            res.status = httplib::StatusCode::RequestTimeout_408;
            res.set_content("", "text/plain");
        }
    });

    svr.Get("/health", [](const httplib::Request& req, httplib::Response& res) {
        res.set_content("Your Server is healthy and running just fine brother!\nYou've come to right place!",
                        "text/plain");
    });

    // Enable thread pool for concurrent request handling
    int num_threads = std::max(2, static_cast<int>(std::thread::hardware_concurrency()));
    svr.new_task_queue = [num_threads] { return new httplib::ThreadPool(num_threads); };

    std::cout << "Server listening on 0.0.0.0:3000 with " << num_threads << " worker threads" << std::endl;
    svr.listen("0.0.0.0", 3000);
    return 0;
}