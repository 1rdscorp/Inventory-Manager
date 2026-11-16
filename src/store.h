// SQLite-backed inventory storage. The database format is the same one the earlier
// Python version used, so an existing inventory.db keeps working.
#pragma once

#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "util.h"

struct sqlite3;

struct ImportSummary {
    int rows = 0, added = 0, updated = 0, unchanged = 0, removed = 0;
    std::vector<std::pair<int, std::string>> skipped;  // row number, reason
    bool dry_run = false;
    std::string sheet;
    json to_json() const;
};

class Store {
public:
    explicit Store(const std::string& path_utf8);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    json list_items();
    json get_item(long long id);
    json create_item(const Record& data);
    json update_item(long long id, const Record& data);
    void delete_item(long long id);
    json adjust(long long id, const json& data);
    json movements(int limit, long long item_id = -1);

    json list_manufacturers();  // [{name, website}] remembered from items
    // Datasheet file attached to an item (one per item, stored in the database).
    json save_datasheet(long long id, const std::string& filename, const std::string& bytes);
    bool get_datasheet(long long id, std::string& filename, std::string& bytes);
    json remove_datasheet(long long id);

    ImportSummary import_records(const std::vector<std::pair<int, Record>>& records, const std::string& mode,
                                 bool dry_run, const std::string& source);
    // First run only: import the spreadsheet that ships with the app. Returns items added, or -1.
    int seed_from_file(const std::string& path_utf8);

private:
    sqlite3* db_ = nullptr;
    std::recursive_mutex mutex_;
};
