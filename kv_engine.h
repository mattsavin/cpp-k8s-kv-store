#pragma once
#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>

struct RecordLocation {
    size_t offset;
    size_t size;
};

class LogStructuredKV {
private:
    std::string filepath;
    std::fstream log_file;
    std::unordered_map<std::string, RecordLocation> index;
    std::mutex db_mutex;

    void write_int(size_t val) {
        log_file.write(reinterpret_cast<const char*>(&val), sizeof(size_t));
    }

    size_t read_int() {
        size_t val;
        log_file.read(reinterpret_cast<char*>(&val), sizeof(size_t));
        return val;
    }

public:
    LogStructuredKV(const std::string& path) : filepath(path) {
        log_file.open(filepath, std::ios::in | std::ios::out | std::ios::app | std::ios::binary);
        if (!log_file.is_open()) {
            log_file.open(filepath, std::ios::out | std::ios::binary);
            log_file.close();
            log_file.open(filepath, std::ios::in | std::ios::out | std::ios::app | std::ios::binary);
        }
        recover();
    }

    void put(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(db_mutex);
        log_file.seekp(0, std::ios::end);
        size_t current_offset = log_file.tellp();

        write_int(key.size());
        write_int(value.size());
        log_file.write(key.data(), key.size());
        log_file.write(value.data(), value.size());
        log_file.flush(); 

        index[key] = {current_offset + sizeof(size_t) * 2 + key.size(), value.size()};
    }

    std::string get(const std::string& key) {
        std::lock_guard<std::mutex> lock(db_mutex);
        auto it = index.find(key);
        if (it == index.end()) return "";

        log_file.seekg(it->second.offset, std::ios::beg);
        std::vector<char> buffer(it->second.size);
        log_file.read(buffer.data(), it->second.size);
        return std::string(buffer.begin(), buffer.end());
    }

    void recover() {
        log_file.seekg(0, std::ios::beg);
        while (log_file.peek() != EOF) {
            size_t start_offset = log_file.tellg();
            size_t key_size = read_int();
            size_t val_size = read_int();

            std::vector<char> key_buf(key_size);
            log_file.read(key_buf.data(), key_size);
            std::string key(key_buf.begin(), key_buf.end());

            size_t value_offset = log_file.tellg();
            log_file.seekg(val_size, std::ios::cur);

            index[key] = {value_offset, val_size};
        }
        log_file.clear(); 
    }
};