#include "httplib.h"
#include "kv_engine.h"
#include "json.hpp"
#include <iostream>
using json = nlohmann::json;

int main()
{
    // Initialize our WAL store. We'll use a local file for now.
    // In K8s, this will map to the mounted Persistent Volume.
    LogStructuredKV kv_store("/data/wal.log");
    httplib::Server svr;

    // POST /put?key=mykey&value=mydata
    svr.Post("/put", [&](const httplib::Request &req, httplib::Response &res)
             {
    json j;
    if (req.has_param("key") && req.has_param("value")) {
        std::string key = req.get_param_value("key");
        std::string value = req.get_param_value("value");
        kv_store.put(key, value);
        
        j["status"] = "success";
        j["message"] = "Key-Value pair stored";
        j["key"] = key;
    } else {
        res.status = 400;
        j["status"] = "error";
        j["message"] = "Missing key or value";
    }
    res.set_content(j.dump(), "application/json"); });

    // GET /get?key=mykey
    svr.Get("/get", [&](const httplib::Request &req, httplib::Response &res)
            {
            if (req.has_param("key")) {
            std::string key = req.get_param_value("key");
            std::string value = kv_store.get(key);
            
            json response; 
            
            if (value.empty()) {
                response["error"] = "Key not found";
                response["key"] = key;
                res.status = 404;
            } else {
                response["key"] = key;
                response["value"] = value;
                response["status"] = "success";
                res.status = 200;
            }
            
            res.set_content(response.dump(), "application/json");
        } else {
            json err = {{"error", "Missing key parameter"}};
            res.status = 400;
            res.set_content(err.dump(), "application/json");
        } });

    // DELETE /delete?key=mykey
    svr.Delete("/delete", [&](const httplib::Request &req, httplib::Response &res)
               {
    json j;
    if (req.has_param("key")) {
        std::string key = req.get_param_value("key");
        kv_store.remove(key);
        
        j["status"] = "success";
        j["message"] = "Tombstone written";
        j["key"] = key;
    } else {
        res.status = 400;
        j["status"] = "error";
        j["message"] = "Missing key";
    }
    res.set_content(j.dump(), "application/json"); });

    // GET /list - List all key-value pairs
    svr.Get("/list", [&](const httplib::Request &req, httplib::Response &res)
            {
    auto data = kv_store.get_all_data();
    json j;
    j["status"] = "success";
    j["count"] = data.size();
    j["data"] = data; // nlohmann/json handles std::map automatically!
    
    res.set_content(j.dump(), "application/json"); });

    std::cout << "Starting KV Store on http://localhost:8080..." << std::endl;
    svr.listen("0.0.0.0", 8080);

    return 0;
}