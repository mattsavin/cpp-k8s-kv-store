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

    /*  Puts a key-value pair into the store. 
        The key and value are written to the end of the log file, 
        and the index is updated with the new location.
    */    
    void put(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(db_mutex); // Ensure thread safety (if we later add concurrency)
        log_file.seekp(0, std::ios::end); // Move to the end of the file for appending
        size_t current_offset = log_file.tellp(); // Get the current offset before writing

        write_int(key.size()); // Write key size
        write_int(value.size()); // Write value size
        log_file.write(key.data(), key.size()); // Write key data
        log_file.write(value.data(), value.size()); // Write value data
        log_file.flush(); // Ensure data is written to disk

        // Update the index with the new location of the value
        index[key] = {current_offset + sizeof(size_t) * 2 + key.size(), value.size()};
    }

    /*  Retrieves the value associated with a given key. */
    std::string get(const std::string& key) {
        std::lock_guard<std::mutex> lock(db_mutex); // Ensure thread safety (if we later add concurrency)
        auto it = index.find(key); // Look up the key in the index
        if (it == index.end()) return ""; // Key not found

        log_file.seekg(it->second.offset, std::ios::beg); // Move to the offset where the value is stored
        std::vector<char> buffer(it->second.size); // Create a buffer to hold the value
        log_file.read(buffer.data(), it->second.size); // Read the value data into the buffer
        return std::string(buffer.begin(), buffer.end()); // Convert buffer to string and return
    }

    /*  Recovers the in-memory index from the log file. 
        This is called during initialization to rebuild the index based on the existing log entries.
    */
    void recover() {
        log_file.seekg(0, std::ios::beg); // Start from the beginning of the file
        while (log_file.peek() != EOF) {
            size_t start_offset = log_file.tellg(); // Get the offset of the current record
            size_t key_size = read_int(); // Read the size of the key
            size_t val_size = read_int(); // Read the size of the value

            std::vector<char> key_buf(key_size); // Create a buffer to hold the key
            log_file.read(key_buf.data(), key_size); // Read the key data into the buffer
            std::string key(key_buf.begin(), key_buf.end()); // Convert buffer to string

            size_t value_offset = log_file.tellg(); // Get the offset where the value starts
            log_file.seekg(val_size, std::ios::cur); // Move the file pointer past the value data

            index[key] = {value_offset, val_size}; // Update the index with the location of the value
        }
        log_file.clear(); 
    }
};