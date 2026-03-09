#include "httplib.h"
#include "kv_engine.h"
#include <iostream>

int main() {
    // Initialize our WAL store. We'll use a local file for now.
    // In K8s, this will map to the mounted Persistent Volume.
    LogStructuredKV kv_store("wal.log");
    httplib::Server svr;

    // POST /put?key=mykey&value=mydata
    svr.Post("/put", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.has_param("key") && req.has_param("value")) {
            std::string key = req.get_param_value("key");
            std::string value = req.get_param_value("value");
            
            kv_store.put(key, value);
            res.set_content("Success", "text/plain");
        } else {
            res.status = 400;
            res.set_content("Missing key or value", "text/plain");
        }
    });

    // GET /get?key=mykey
    svr.Get("/get", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.has_param("key")) {
            std::string key = req.get_param_value("key");
            std::string value = kv_store.get(key);
            
            if (value.empty()) {
                res.status = 404;
                res.set_content("Key not found", "text/plain");
            } else {
                res.set_content(value, "text/plain");
            }
        } else {
            res.status = 400;
            res.set_content("Missing key", "text/plain");
        }
    });

    std::cout << "Starting KV Store on http://localhost:8081..." << std::endl;
    svr.listen("0.0.0.0", 8081);

    return 0;
}