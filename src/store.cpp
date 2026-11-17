#include "store.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

#include <sqlite3.h>

#include "spreadsheet.h"

namespace {

const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS items (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    sku           TEXT NOT NULL UNIQUE COLLATE NOCASE,
    name          TEXT NOT NULL,
    category      TEXT NOT NULL DEFAULT '',
    location      TEXT NOT NULL DEFAULT '',
    supplier      TEXT NOT NULL DEFAULT '',
    quantity      REAL NOT NULL DEFAULT 0,
    reorder_level REAL NOT NULL DEFAULT 0,
    unit_cost     REAL NOT NULL DEFAULT 0,
    unit_price    REAL NOT NULL DEFAULT 0,
    notes         TEXT NOT NULL DEFAULT '',
    created_at    TEXT NOT NULL,
    updated_at    TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS movements (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    item_id        INTEGER,
    sku            TEXT NOT NULL,
    item_name      TEXT NOT NULL,
    change         REAL NOT NULL,
    quantity_after REAL NOT NULL,
    kind           TEXT NOT NULL,
    note           TEXT NOT NULL DEFAULT '',
    created_at     TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_movements_item ON movements(item_id);
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE IF NOT EXISTS suppliers (
    name       TEXT PRIMARY KEY COLLATE NOCASE,
    website    TEXT NOT NULL DEFAULT '',
    updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS attachments (
    item_id     INTEGER PRIMARY KEY,
    filename    TEXT NOT NULL,
    size        INTEGER NOT NULL,
    data        BLOB NOT NULL,
    uploaded_at TEXT NOT NULL
);
)SQL";

// Columns added since version 2.0 - added to older databases when they're opened.
// (Version 2.1's manufacturer / manufacturer_url columns are left in place, unused, so no data is lost.)
const char* kNewItemColumns[] = {"supplier_url", "datasheet_url"};

// Every item query reads these columns, in this order (see read_row).
const char* kItemSelect =
    "SELECT items.id, items.sku, items.name, items.category, items.location, items.supplier, items.quantity, "
    "items.reorder_level, items.unit_cost, items.unit_price, items.notes, items.created_at, items.updated_at, "
    "items.supplier_url, items.datasheet_url, a.filename, a.size "
    "FROM items LEFT JOIN attachments a ON a.item_id = items.id";

void exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "database error";
        sqlite3_free(err);
        throw std::runtime_error(msg);
    }
}

class Stmt {
public:
    Stmt(sqlite3* db, const std::string& sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s_, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Stmt() { sqlite3_finalize(s_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Stmt& bind(int i, const std::string& v) {
        sqlite3_bind_text(s_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& bind(int i, double v) { sqlite3_bind_double(s_, i, v); return *this; }
    Stmt& bind(int i, long long v) { sqlite3_bind_int64(s_, i, v); return *this; }
    Stmt& bind(int i, int v) { sqlite3_bind_int64(s_, i, v); return *this; }
    Stmt& bind_blob(int i, const std::string& v) {
        sqlite3_bind_blob(s_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    std::string blob(int c) {
        auto p = static_cast<const char*>(sqlite3_column_blob(s_, c));
        return p ? std::string(p, sqlite3_column_bytes(s_, c)) : std::string();
    }
    Stmt& bind_null(int i) { sqlite3_bind_null(s_, i); return *this; }

    bool step() {
        int rc = sqlite3_step(s_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    std::string text(int c) {
        auto p = reinterpret_cast<const char*>(sqlite3_column_text(s_, c));
        return p ? std::string(p, sqlite3_column_bytes(s_, c)) : std::string();
    }
    double dbl(int c) { return sqlite3_column_double(s_, c); }
    long long i64(int c) { return sqlite3_column_int64(s_, c); }
    bool is_null(int c) { return sqlite3_column_type(s_, c) == SQLITE_NULL; }

private:
    sqlite3* db_;
    sqlite3_stmt* s_ = nullptr;
};

// BEGIN IMMEDIATE ... COMMIT; rolls back if commit() isn't reached.
class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) { exec(db_, "BEGIN IMMEDIATE"); }
    ~Transaction() {
        if (!done_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    void commit() { exec(db_, "COMMIT"); done_ = true; }

private:
    sqlite3* db_;
    bool done_ = false;
};

struct ItemRow {
    long long id = 0;
    Item item;
    std::string created_at, updated_at;
    std::string file_name;  // attached datasheet file, if any
    long long file_size = 0;
};

ItemRow read_row(Stmt& s) {
    ItemRow r;
    r.id = s.i64(0);
    r.item.sku = s.text(1);
    r.item.name = s.text(2);
    r.item.category = s.text(3);
    r.item.location = s.text(4);
    r.item.supplier = s.text(5);
    r.item.quantity = s.dbl(6);
    r.item.reorder_level = s.dbl(7);
    r.item.unit_cost = s.dbl(8);
    r.item.unit_price = s.dbl(9);
    r.item.notes = s.text(10);
    r.created_at = s.text(11);
    r.updated_at = s.text(12);
    r.item.supplier_url = s.text(13);
    r.item.datasheet_url = s.text(14);
    if (!s.is_null(15)) {
        r.file_name = s.text(15);
        r.file_size = s.i64(16);
    }
    return r;
}

json item_json(const ItemRow& r) {
    const Item& it = r.item;
    // Same rules as the spreadsheet: Total Value = Quantity x Unit Cost,
    // Status = "Reorder" when Quantity <= Reorder Level.
    return {{"id", r.id},
            {"sku", it.sku},
            {"name", it.name},
            {"category", it.category},
            {"location", it.location},
            {"supplier", it.supplier},
            {"quantity", tidy_number(it.quantity)},
            {"reorder_level", tidy_number(it.reorder_level)},
            {"unit_cost", tidy_number(it.unit_cost)},
            {"unit_price", tidy_number(it.unit_price)},
            {"notes", it.notes},
            {"supplier_url", it.supplier_url},
            {"datasheet_url", it.datasheet_url},
            {"datasheet_file", r.file_name},
            {"datasheet_size", r.file_size},
            {"total_value", std::round(it.quantity * it.unit_cost * 100.0) / 100.0},
            {"status", it.quantity <= it.reorder_level ? "Reorder" : "OK"},
            {"created_at", r.created_at},
            {"updated_at", r.updated_at}};
}

bool same(const Item& a, const Item& b) {
    auto eq = [](double x, double y) { return std::fabs(x - y) < 1e-9; };
    return a.sku == b.sku && a.name == b.name && a.category == b.category && a.location == b.location &&
           a.supplier == b.supplier && a.notes == b.notes && a.supplier_url == b.supplier_url &&
           a.datasheet_url == b.datasheet_url && eq(a.quantity, b.quantity) &&
           eq(a.reorder_level, b.reorder_level) && eq(a.unit_cost, b.unit_cost) && eq(a.unit_price, b.unit_price);
}

ItemRow fetch(sqlite3* db, long long id) {
    Stmt s(db, std::string(kItemSelect) + " WHERE items.id = ?");
    s.bind(1, id);
    if (!s.step()) throw NotFound("That item no longer exists. It may have been deleted.");
    return read_row(s);
}

bool fetch_by_sku(sqlite3* db, const std::string& sku, ItemRow& out) {
    Stmt s(db, std::string(kItemSelect) + " WHERE items.sku = ?");
    s.bind(1, sku);
    if (!s.step()) return false;
    out = read_row(s);
    return true;
}

void check_sku_free(sqlite3* db, const std::string& sku, long long exclude_id = -1) {
    Stmt s(db, "SELECT name FROM items WHERE sku = ? AND id IS NOT ?");
    s.bind(1, sku);
    if (exclude_id < 0) s.bind_null(2); else s.bind(2, exclude_id);
    if (s.step()) throw Conflict("SKU \"" + sku + "\" is already used by \"" + s.text(0) + "\"");
}

void log_movement(sqlite3* db, long long item_id, const std::string& sku, const std::string& name, double change,
                  double after, const std::string& kind, const std::string& note = "") {
    Stmt s(db,
           "INSERT INTO movements (item_id, sku, item_name, change, quantity_after, kind, note, created_at) "
           "VALUES (?,?,?,?,?,?,?,?)");
    s.bind(1, item_id).bind(2, sku).bind(3, name).bind(4, round4(change)).bind(5, round4(after));
    s.bind(6, kind).bind(7, note).bind(8, now_iso());
    s.step();
}

// Supplier websites are remembered: an item that names a known supplier but has no website
// gets the saved one, and an item that has both updates the saved website.
void apply_supplier(sqlite3* db, Item& it) {
    if (it.supplier.empty()) return;
    if (it.supplier_url.empty()) {
        Stmt s(db, "SELECT website FROM suppliers WHERE name = ?");
        s.bind(1, it.supplier);
        if (s.step()) it.supplier_url = s.text(0);
    } else {
        Stmt s(db,
               "INSERT INTO suppliers (name, website, updated_at) VALUES (?,?,?) "
               "ON CONFLICT(name) DO UPDATE SET website = excluded.website, updated_at = excluded.updated_at");
        s.bind(1, it.supplier).bind(2, it.supplier_url).bind(3, now_iso());
        s.step();
    }
}

void delete_attachment(sqlite3* db, long long item_id) {
    Stmt s(db, "DELETE FROM attachments WHERE item_id = ?");
    s.bind(1, item_id);
    s.step();
}

long long insert_item(sqlite3* db, const Item& it, const std::string& kind = "created", const std::string& note = "") {
    std::string ts = now_iso();
    Stmt s(db,
           "INSERT INTO items (sku, name, category, location, supplier, quantity, reorder_level, unit_cost, "
           "unit_price, notes, created_at, updated_at, supplier_url, datasheet_url) "
           "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    s.bind(1, it.sku).bind(2, it.name).bind(3, it.category).bind(4, it.location).bind(5, it.supplier);
    s.bind(6, it.quantity).bind(7, it.reorder_level).bind(8, it.unit_cost).bind(9, it.unit_price);
    s.bind(10, it.notes).bind(11, ts).bind(12, ts);
    s.bind(13, it.supplier_url).bind(14, it.datasheet_url);
    s.step();
    long long id = sqlite3_last_insert_rowid(db);
    log_movement(db, id, it.sku, it.name, it.quantity, it.quantity, kind, note);
    return id;
}

void update_row(sqlite3* db, const ItemRow& row, const Item& it, const std::string& kind, const std::string& note) {
    Stmt s(db,
           "UPDATE items SET sku=?, name=?, category=?, location=?, supplier=?, quantity=?, reorder_level=?, "
           "unit_cost=?, unit_price=?, notes=?, updated_at=?, supplier_url=?, datasheet_url=? WHERE id=?");
    s.bind(1, it.sku).bind(2, it.name).bind(3, it.category).bind(4, it.location).bind(5, it.supplier);
    s.bind(6, it.quantity).bind(7, it.reorder_level).bind(8, it.unit_cost).bind(9, it.unit_price);
    s.bind(10, it.notes).bind(11, now_iso()).bind(12, it.supplier_url).bind(13, it.datasheet_url).bind(14, row.id);
    s.step();
    double change = it.quantity - row.item.quantity;
    if (std::fabs(change) > 1e-9) log_movement(db, row.id, it.sku, it.name, change, it.quantity, kind, note);
}

}  // namespace

json ImportSummary::to_json() const {
    json sk = json::array();
    for (const auto& [row, reason] : skipped) sk.push_back({{"row", row}, {"reason", reason}});
    return {{"rows", rows},       {"added", added},     {"updated", updated}, {"unchanged", unchanged},
            {"removed", removed}, {"skipped", sk},      {"dry_run", dry_run}, {"sheet", sheet}};
}

Store::Store(const std::string& path_utf8) : path_(path_utf8) {
    if (sqlite3_open_v2(path_utf8.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(msg);
    }
    sqlite3_busy_timeout(db_, 5000);
    ensure_schema();
}

void Store::ensure_schema() {
    exec(db_, kSchema);
    // Bring databases from older versions up to date.
    std::set<std::string> have;
    {
        Stmt cols(db_, "PRAGMA table_info(items)");
        while (cols.step()) have.insert(cols.text(1));
    }
    for (const char* col : kNewItemColumns)
        if (!have.count(col)) exec(db_, (std::string("ALTER TABLE items ADD COLUMN ") + col + " TEXT NOT NULL DEFAULT ''").c_str());
}

Store::~Store() { sqlite3_close(db_); }

// ------------------------------------------------------------------ settings

namespace {

const std::vector<std::string> kCurrencies = {"USD", "EUR", "GBP", "CAD", "AUD", "NZD", "CHF", "JPY", "CNY", "INR",
                                              "MXN", "BRL", "ZAR", "SEK", "NOK", "DKK", "PLN", "SGD", "HKD", "AED"};

json default_settings() {
    return {{"currency", "USD"},     {"theme", "system"},  {"default_reorder_level", 0},
            {"default_category", ""}, {"auto_backup", true}, {"backup_keep", 10}};
}

// Checks one setting; throws ApiError with a readable message if it's not allowed.
json check_setting(const std::string& key, const json& v) {
    if (key == "currency") {
        std::string c = v.is_string() ? v.get<std::string>() : "";
        if (std::find(kCurrencies.begin(), kCurrencies.end(), c) == kCurrencies.end()) throw ApiError("Unknown currency");
        return c;
    }
    if (key == "theme") {
        std::string t = v.is_string() ? v.get<std::string>() : "";
        if (t != "system" && t != "light" && t != "dark") throw ApiError("Unknown theme");
        return t;
    }
    if (key == "default_reorder_level") return tidy_number(parse_number(Value::from_json(v), "Default reorder level"));
    if (key == "default_category") return clean_text(Value::from_json(v), "category");
    if (key == "auto_backup") {
        if (!v.is_boolean()) throw ApiError("Automatic backup must be on or off");
        return v;
    }
    if (key == "backup_keep") {
        double n = parse_number(Value::from_json(v), "Backups to keep");
        if (n < 1 || n > 100 || n != std::floor(n)) throw ApiError("Backups to keep must be a whole number from 1 to 100");
        return static_cast<int>(n);
    }
    throw ApiError("Unknown setting: " + key);
}

std::filesystem::path u8path(const std::string& s) {
    return std::filesystem::path(reinterpret_cast<const char8_t*>(s.c_str()));
}

std::string path_utf8(const std::filesystem::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// "2026-11-16_143005" - sortable, safe in file names.
std::string file_stamp() {
    std::string s = now_iso().substr(0, 19);
    std::replace(s.begin(), s.end(), 'T', '_');
    s.erase(std::remove(s.begin(), s.end(), ':'), s.end());
    return s;
}

void copy_database(sqlite3* from, sqlite3* to) {
    sqlite3_backup* b = sqlite3_backup_init(to, "main", from, "main");
    if (!b) throw std::runtime_error(sqlite3_errmsg(to));
    int rc = sqlite3_backup_step(b, -1);
    sqlite3_backup_finish(b);
    if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errstr(rc));
}

std::filesystem::path temp_file(const char* prefix) {
    static std::atomic<unsigned> counter{0};
    auto t = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           (std::string(prefix) + std::to_string(t) + "-" + std::to_string(counter++) + ".db");
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

json Store::get_settings() {
    std::lock_guard lock(mutex_);
    json s = default_settings();
    Stmt q(db_, "SELECT value FROM settings WHERE key = 'app'");
    if (q.step()) {
        json saved = json::parse(q.text(0), nullptr, false);
        if (saved.is_object())
            for (auto& [k, v] : saved.items()) {
                if (!s.contains(k)) continue;
                try { s[k] = check_setting(k, v); } catch (const ApiError&) { /* keep the default */ }
            }
    }
    return s;
}

json Store::save_settings(const json& changes) {
    if (!changes.is_object()) throw ApiError("Invalid settings");
    json s = get_settings();
    for (auto& [k, v] : changes.items()) s[k] = check_setting(k, v);
    std::lock_guard lock(mutex_);
    Stmt q(db_, "INSERT INTO settings (key, value) VALUES ('app', ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    q.bind(1, s.dump());
    q.step();
    return s;
}

// ------------------------------------------------------------------ backup & restore

std::string Store::backups_dir() const { return path_utf8(u8path(path_).parent_path() / "Backups"); }

void Store::backup_to(const std::string& dest_utf8) {
    std::lock_guard lock(mutex_);
    std::error_code ec;
    std::filesystem::create_directories(u8path(dest_utf8).parent_path(), ec);
    sqlite3* out = nullptr;
    if (sqlite3_open_v2(dest_utf8.c_str(), &out, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        std::string msg = out ? sqlite3_errmsg(out) : "out of memory";
        sqlite3_close(out);
        throw std::runtime_error("Couldn't create the backup file: " + msg);
    }
    try {
        copy_database(db_, out);
    } catch (...) {
        sqlite3_close(out);
        throw;
    }
    sqlite3_close(out);
}

std::string Store::backup_bytes() {
    auto tmp = temp_file("im-backup-");
    backup_to(path_utf8(tmp));
    std::string bytes = read_file(tmp);
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    return bytes;
}

json Store::list_backups() {
    json out = json::array();
    std::error_code ec;
    std::vector<std::filesystem::directory_entry> files;
    for (const auto& e : std::filesystem::directory_iterator(u8path(backups_dir()), ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".db") files.push_back(e);
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.last_write_time() > b.last_write_time(); });
    for (const auto& e : files) {
        std::string name = path_utf8(e.path().filename());
        auto when = std::chrono::clock_cast<std::chrono::system_clock>(e.last_write_time(ec));
        long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()).count();
        std::string kind = name.rfind("auto-", 0) == 0 ? "auto" : name.rfind("before-restore-", 0) == 0 ? "before-restore" : "other";
        out.push_back({{"name", name}, {"size", static_cast<long long>(e.file_size(ec))}, {"modified", ms}, {"kind", kind}});
    }
    return out;
}

int Store::restore_from_bytes(const std::string& bytes) {
    const std::string not_backup = "This file isn't an Inventory Manager backup.";
    if (bytes.size() < 100 || bytes.compare(0, 16, std::string("SQLite format 3\0", 16)) != 0) throw ApiError(not_backup);

    auto tmp = temp_file("im-restore-");
    {
        std::ofstream outf(tmp, std::ios::binary);
        outf.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!outf) throw std::runtime_error("Couldn't write a temporary file");
    }
    sqlite3* src = nullptr;
    auto cleanup = [&] {
        sqlite3_close(src);
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
    };
    try {
        if (sqlite3_open_v2(path_utf8(tmp).c_str(), &src, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) throw ApiError(not_backup);
        // Must be intact and look like an inventory database.
        try {
            Stmt check(src, "PRAGMA quick_check");
            if (!check.step() || check.text(0) != "ok") throw ApiError("That backup file is damaged and can't be restored.");
            std::set<std::string> cols;
            Stmt info(src, "PRAGMA table_info(items)");
            while (info.step()) cols.insert(info.text(1));
            Stmt mv(src, "SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'movements'");
            if (!cols.count("sku") || !cols.count("name") || !cols.count("quantity") || !mv.step()) throw ApiError(not_backup);
        } catch (const std::runtime_error& e) {
            if (dynamic_cast<const ApiError*>(&e)) throw;
            throw ApiError(not_backup);
        }

        std::lock_guard lock(mutex_);
        // Keep a copy of what's being replaced, so a restore can always be undone.
        backup_to(path_utf8(u8path(backups_dir()) / ("before-restore-" + file_stamp() + ".db")));
        copy_database(src, db_);
        ensure_schema();
        Stmt c(db_, "SELECT COUNT(*) FROM items");
        int count = c.step() ? static_cast<int>(c.i64(0)) : 0;
        cleanup();
        return count;
    } catch (...) {
        cleanup();
        throw;
    }
}

int Store::restore_from_backups_dir(const std::string& name) {
    if (name.empty() || name.find_first_of("/\\:") != std::string::npos || name.find("..") != std::string::npos ||
        name.size() < 4 || name.compare(name.size() - 3, 3, ".db") != 0)
        throw ApiError("Unknown backup");
    auto p = u8path(backups_dir()) / u8path(name);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) throw NotFound("That backup no longer exists.");
    return restore_from_bytes(read_file(p));
}

bool Store::auto_backup_if_due() {
    json s = get_settings();
    if (!s.value("auto_backup", true)) return false;
    int keep = s.value("backup_keep", 10);
    auto dir = u8path(backups_dir());
    std::error_code ec;
    std::vector<std::filesystem::directory_entry> autos;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        std::string n = path_utf8(e.path().filename());
        if (n.rfind("auto-", 0) == 0 && e.path().extension() == ".db") autos.push_back(e);
    }
    std::sort(autos.begin(), autos.end(), [](const auto& a, const auto& b) { return a.path().filename() > b.path().filename(); });
    bool made = false;
    if (autos.empty() || std::filesystem::file_time_type::clock::now() - autos.front().last_write_time(ec) > std::chrono::hours(20)) {
        backup_to(path_utf8(dir / ("auto-" + file_stamp() + ".db")));
        made = true;
        autos.clear();
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            std::string n = path_utf8(e.path().filename());
            if (n.rfind("auto-", 0) == 0 && e.path().extension() == ".db") autos.push_back(e);
        }
        std::sort(autos.begin(), autos.end(), [](const auto& a, const auto& b) { return a.path().filename() > b.path().filename(); });
    }
    for (size_t i = static_cast<size_t>(keep); i < autos.size(); ++i) std::filesystem::remove(autos[i].path(), ec);
    return made;
}

json Store::list_items() {
    std::lock_guard lock(mutex_);
    Stmt s(db_, std::string(kItemSelect) + " ORDER BY items.sku COLLATE NOCASE");
    json out = json::array();
    while (s.step()) out.push_back(item_json(read_row(s)));
    return out;
}

json Store::list_suppliers() {
    std::lock_guard lock(mutex_);
    Stmt s(db_, "SELECT name, website FROM suppliers ORDER BY name COLLATE NOCASE");
    json out = json::array();
    while (s.step()) out.push_back({{"name", s.text(0)}, {"website", s.text(1)}});
    return out;
}

json Store::save_datasheet(long long id, const std::string& filename, const std::string& bytes) {
    if (bytes.empty()) throw ApiError("That file is empty");
    std::string name = filename.empty() ? "datasheet" : filename;
    std::lock_guard lock(mutex_);
    Transaction tx(db_);
    fetch(db_, id);
    Stmt s(db_,
           "INSERT INTO attachments (item_id, filename, size, data, uploaded_at) VALUES (?,?,?,?,?) "
           "ON CONFLICT(item_id) DO UPDATE SET filename = excluded.filename, size = excluded.size, "
           "data = excluded.data, uploaded_at = excluded.uploaded_at");
    s.bind(1, id).bind(2, name).bind(3, static_cast<long long>(bytes.size())).bind_blob(4, bytes).bind(5, now_iso());
    s.step();
    tx.commit();
    return item_json(fetch(db_, id));
}

bool Store::get_datasheet(long long id, std::string& filename, std::string& bytes) {
    std::lock_guard lock(mutex_);
    Stmt s(db_, "SELECT filename, data FROM attachments WHERE item_id = ?");
    s.bind(1, id);
    if (!s.step()) return false;
    filename = s.text(0);
    bytes = s.blob(1);
    return true;
}

json Store::remove_datasheet(long long id) {
    std::lock_guard lock(mutex_);
    Transaction tx(db_);
    fetch(db_, id);
    delete_attachment(db_, id);
    tx.commit();
    return item_json(fetch(db_, id));
}

json Store::get_item(long long id) {
    std::lock_guard lock(mutex_);
    return item_json(fetch(db_, id));
}

json Store::create_item(const Record& data) {
    Item it = validate_item(data);
    std::lock_guard lock(mutex_);
    Transaction tx(db_);
    check_sku_free(db_, it.sku);
    apply_supplier(db_, it);
    long long id = insert_item(db_, it);
    tx.commit();
    return item_json(fetch(db_, id));
}

json Store::update_item(long long id, const Record& data) {
    std::lock_guard lock(mutex_);
    Transaction tx(db_);
    ItemRow row = fetch(db_, id);
    Record merged = item_to_record(row.item);
    for (const auto& [k, v] : data) merged[k] = v;
    Item it = validate_item(merged);
    check_sku_free(db_, it.sku, id);
    apply_supplier(db_, it);
    update_row(db_, row, it, "edited", "Quantity changed in item details");
    tx.commit();
    return item_json(fetch(db_, id));
}

void Store::delete_item(long long id) {
    std::lock_guard lock(mutex_);
    Transaction tx(db_);
    ItemRow row = fetch(db_, id);
    Stmt s(db_, "DELETE FROM items WHERE id = ?");
    s.bind(1, id);
    s.step();
    delete_attachment(db_, id);
    log_movement(db_, id, row.item.sku, row.item.name, -row.item.quantity, 0, "deleted", "Item deleted");
    tx.commit();
}

json Store::adjust(long long id, const json& data) {
    if (!data.is_object()) throw ApiError("Invalid request");
    json amount_j = data.value("amount", json());
    if (amount_j.is_null() || (amount_j.is_string() && amount_j.get<std::string>().empty()))
        throw ApiError("Enter a quantity");
    std::string mode = data.contains("mode") && data["mode"].is_string() ? data["mode"].get<std::string>() : "";
    double amount = parse_number(Value::from_json(amount_j), "Quantity");
    std::string note = utf8_prefix(clean_text(Value::from_json(data.value("note", json())), "notes"), 300);

    std::lock_guard lock(mutex_);
    Transaction tx(db_);
    ItemRow row = fetch(db_, id);
    double current = row.item.quantity, next = 0;
    std::string kind;
    if (mode == "add") {
        if (amount <= 0) throw ApiError("Enter how many units you received");
        next = current + amount;
        kind = "received";
    } else if (mode == "remove") {
        if (amount <= 0) throw ApiError("Enter how many units to remove");
        if (amount > current + 1e-9)
            throw ApiError("Only " + number_text(round4(current)) + " in stock, so " + number_text(round4(amount)) +
                           " can't be removed");
        next = std::max(0.0, current - amount);
        kind = "removed";
    } else if (mode == "set") {
        next = amount;
        kind = "counted";
    } else {
        throw ApiError("Unknown adjustment type");
    }
    next = round4(next);
    Stmt s(db_, "UPDATE items SET quantity = ?, updated_at = ? WHERE id = ?");
    s.bind(1, next).bind(2, now_iso()).bind(3, id);
    s.step();
    log_movement(db_, id, row.item.sku, row.item.name, next - current, next, kind, note);
    tx.commit();
    return item_json(fetch(db_, id));
}

json Store::movements(int limit, long long item_id) {
    limit = std::max(1, std::min(limit, 20000));
    std::lock_guard lock(mutex_);
    std::string sql =
        "SELECT id, item_id, sku, item_name, change, quantity_after, kind, note, created_at FROM movements ";
    sql += item_id >= 0 ? "WHERE item_id = ? ORDER BY id DESC LIMIT ?" : "ORDER BY id DESC LIMIT ?";
    Stmt s(db_, sql);
    if (item_id >= 0) s.bind(1, item_id).bind(2, limit); else s.bind(1, limit);
    json out = json::array();
    while (s.step()) {
        out.push_back({{"id", s.i64(0)},
                       {"item_id", s.is_null(1) ? json() : json(s.i64(1))},
                       {"sku", s.text(2)},
                       {"item_name", s.text(3)},
                       {"change", tidy_number(s.dbl(4))},
                       {"quantity_after", tidy_number(s.dbl(5))},
                       {"kind", s.text(6)},
                       {"note", s.text(7)},
                       {"created_at", s.text(8)}});
    }
    return out;
}

ImportSummary Store::import_records(const std::vector<std::pair<int, Record>>& records, const std::string& mode,
                                    bool dry_run, const std::string& source) {
    if (mode != "merge" && mode != "replace") throw ApiError("Unknown import mode");
    ImportSummary sum;
    sum.rows = static_cast<int>(records.size());
    sum.dry_run = dry_run;
    std::string note = "Imported from " + source;

    std::lock_guard lock(mutex_);
    Transaction tx(db_);
    if (mode == "replace") {
        Stmt all(db_, kItemSelect);
        std::vector<ItemRow> rows;
        while (all.step()) rows.push_back(read_row(all));
        for (const auto& r : rows) {
            log_movement(db_, r.id, r.item.sku, r.item.name, -r.item.quantity, 0, "deleted",
                         "Removed when replacing inventory from " + source);
            ++sum.removed;
        }
        exec(db_, "DELETE FROM items");
        exec(db_, "DELETE FROM attachments");
    }
    std::map<std::string, int> seen;  // lower-case SKU -> first row
    for (const auto& [rownum, raw] : records) {
        ItemRow existing;
        bool found = false;
        Item it;
        std::string key;
        try {
            auto sku_it = raw.find("sku");
            std::string sku = clean_text(sku_it == raw.end() ? Value() : sku_it->second, "sku");
            if (sku.empty()) throw ApiError("Missing SKU");
            key = lower_ascii(sku);
            if (auto s = seen.find(key); s != seen.end())
                throw ApiError("Duplicate SKU \"" + sku + "\" (first seen on row " + std::to_string(s->second) + ")");
            found = fetch_by_sku(db_, sku, existing);
            Record base = found ? item_to_record(existing.item) : Record{};
            for (const auto& [k, v] : raw) base[k] = v;
            it = validate_item(base);
            apply_supplier(db_, it);
        } catch (const ApiError& e) {
            sum.skipped.emplace_back(rownum, e.what());
            continue;
        }
        seen[key] = rownum;
        if (!found) {
            insert_item(db_, it, "imported", note);
            ++sum.added;
        } else if (!same(it, existing.item)) {
            update_row(db_, existing, it, "imported", note);
            ++sum.updated;
        } else {
            ++sum.unchanged;
        }
    }
    if (!dry_run) tx.commit();  // a preview simply rolls back
    return sum;
}

int Store::seed_from_file(const std::string& path_utf8) {
    {
        std::lock_guard lock(mutex_);
        Stmt done(db_, "SELECT value FROM meta WHERE key = 'seeded'");
        if (done.step()) return -1;
    }
    int added = -1;
    long long count = 0;
    {
        std::lock_guard lock(mutex_);
        Stmt c(db_, "SELECT COUNT(*) FROM items");
        if (c.step()) count = c.i64(0);
    }
    std::filesystem::path p(reinterpret_cast<const char8_t*>(path_utf8.c_str()));
    std::error_code ec;
    if (count == 0 && std::filesystem::exists(p, ec)) {
        std::ifstream in(p, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto [sheet, records] = read_spreadsheet(bytes);
        auto u8name = p.filename().u8string();
        added = import_records(records, "merge", false, std::string(u8name.begin(), u8name.end())).added;
    }
    std::lock_guard lock(mutex_);
    Stmt s(db_, "INSERT OR REPLACE INTO meta (key, value) VALUES ('seeded', ?)");
    s.bind(1, now_iso());
    s.step();
    return added;
}
