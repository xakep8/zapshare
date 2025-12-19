#define CPPHTTPLIB_OPENSSL_SUPPORT

#include <iostream>

#include "database.hpp"
#include "httplib.h"

int main() {
    DB::init();

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

    svr.Post(R"(/lookup/(\w+))", [](const httplib::Request& req, httplib::Response& res) {
        const std::string_view secret = req.matches[1];
        res.set_content("Hello World!", "text/plain");
    });

    svr.Get("/", [](const httplib::Request& req, httplib::Response& res) {
        res.set_content("You've come to right place!", "text/plain");
    });

    svr.listen("0.0.0.0", 3000);
    return 0;
}