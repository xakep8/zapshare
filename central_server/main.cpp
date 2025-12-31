#define CPPHTTPLIB_OPENSSL_SUPPORT

#include <iostream>
#include <thread>

#include "database.hpp"
#include "httplib.h"
#include "json.hpp"

using json = nlohmann::json;

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