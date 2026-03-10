#pragma once
#include <iostream>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <map>
#include <mutex>
#include <shared_mutex>

struct RecordLocation
{
    size_t offset;
    size_t size;
};

class LogStructuredKV
{
private:
    std::string filepath;
    std::fstream log_file;
    std::unordered_map<std::string, RecordLocation> index;
    std::shared_mutex db_mutex;

    void write_int(size_t val)
    {
        log_file.write(reinterpret_cast<const char *>(&val), sizeof(size_t));
    }

    size_t read_int()
    {
        size_t val;
        log_file.read(reinterpret_cast<char *>(&val), sizeof(size_t));
        return val;
    }

public:
    /*  Operation codes for the log entries.
        This can be extended in the future to support more operations.
    */
    enum class OpCode : uint8_t
    {
        PUT = 0x01,
        DELETE = 0x02
    };

    LogStructuredKV(const std::string &path) : filepath(path)
    {
        log_file.open(filepath, std::ios::in | std::ios::out | std::ios::app | std::ios::binary);
        if (!log_file.is_open())
        {
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
    void put(const std::string &key, const std::string &value)
    {
        std::unique_lock<std::shared_mutex> lock(db_mutex); // Ensure thread safety
        log_file.seekp(0, std::ios::end);                   // Move to the end of the file for appending
        size_t current_offset = log_file.tellp();           // Get the current offset before writing

        OpCode op_code = OpCode::PUT;                                             // Operation code for PUT
        log_file.write(reinterpret_cast<const char *>(&op_code), sizeof(OpCode)); // Write the operation code
        write_int(key.size());                                                    // Write key size
        write_int(value.size());                                                  // Write value size
        log_file.write(key.data(), key.size());                                   // Write key data
        log_file.write(value.data(), value.size());                               // Write value data
        log_file.flush();                                                         // Ensure data is written to disk

        // Update the index with the new location of the value
        index[key] = {current_offset + sizeof(OpCode) + sizeof(size_t) * 2 + key.size(), value.size()};
    }

    /*  Retrieves the value associated with a given key.
        Returns std::nullopt when the key is not found; returns
        an engaged optional containing an empty string when the
        stored value is empty.
    */
    std::optional<std::string> get(const std::string &key)
    {
        std::shared_lock<std::shared_mutex> lock(db_mutex); // Ensure thread safety
        auto it = index.find(key);                          // Look up the key in the index
        if (it == index.end())
            return std::nullopt; // Key not found

        // Use a local ifstream for reads to avoid races on shared fstream state
        std::ifstream in(filepath, std::ios::in | std::ios::binary);
        if (!in.is_open())
            return std::nullopt; // Treat inability to open as not-found / error

        in.seekg(it->second.offset, std::ios::beg); // Move to the offset where the value is stored
        std::vector<char> buffer(it->second.size);  // Create a buffer to hold the value
        if (it->second.size > 0)
            in.read(buffer.data(), it->second.size);                                  // Read the value data into the buffer
        return std::optional<std::string>(std::string(buffer.begin(), buffer.end())); // Return value (may be empty)
    }

    void remove(const std::string &key)
    {

        std::unique_lock<std::shared_mutex> lock(db_mutex); // Ensure thread safety

        auto it = index.find(key); // Look up the key in the index
        if (it != index.end())
        {
            OpCode op_code = OpCode::DELETE;          // Operation code for DELETE
            log_file.seekp(0, std::ios::end);         // Move to the end of the file for appending
            size_t current_offset = log_file.tellp(); // Get the current offset before writing

            log_file.write(reinterpret_cast<const char *>(&op_code), sizeof(OpCode)); // Write the operation code
            write_int(key.size());                                                    // Write key size
            write_int(0);                                                             // Value size is 0 for delete operations
            log_file.write(key.data(), key.size());                                   // Write key data
            log_file.flush();                                                         // Ensure data is written to disk

            index.erase(it); // Remove the key from the index
        }
    }

    // Returns all current key-value pairs as a map
    std::map<std::string, std::string> get_all_data()
    {
        std::shared_lock<std::shared_mutex> lock(db_mutex); // Ensure thread safety

        std::map<std::string, std::string> all_data; // Map to hold all key-value pairs

        std::ifstream in(filepath, std::ios::in | std::ios::binary);
        if (!in.is_open())
            return all_data; // return empty map if we can't open file

        for (const auto &[key, loc] : index)
        {
            in.seekg(loc.offset, std::ios::beg); // Move to the offset where the value is stored
            std::vector<char> buffer(loc.size);
            if (loc.size > 0)
                in.read(buffer.data(), loc.size);
            all_data[key] = std::string(buffer.begin(), buffer.end());
        }
        return all_data;
    }

    /*  Recovers the in-memory index from the log file.
        This is called during initialization to rebuild the index based on the existing log entries.
    */
    void recover()
    {
        log_file.clear();                 // Clear any error flags
        log_file.seekg(0, std::ios::beg); // Start from the beginning of the file

        OpCode op_code; // Variable to hold the operation code while reading the log
        while (log_file.read(reinterpret_cast<char *>(&op_code), sizeof(OpCode)))
        {
            size_t key_size = read_int(); // Read the size of the key
            size_t val_size = read_int(); // Read the size of the value

            std::vector<char> key_buf(key_size);             // Create a buffer to hold the key
            log_file.read(key_buf.data(), key_size);         // Read the key data into the buffer
            std::string key(key_buf.begin(), key_buf.end()); // Convert buffer to string

            if (op_code == OpCode::PUT)
            {
                size_t value_offset = log_file.tellg(); // Get the offset where the value is stored
                index[key] = {value_offset, val_size};  // Update the index with the location of the value
            }
            else if (op_code == OpCode::DELETE)
            {
                index.erase(key); // Remove the key from the index
            }
            log_file.seekg(val_size, std::ios::cur); // Move the file pointer
        }
        log_file.clear();
    }
};