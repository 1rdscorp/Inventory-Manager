#include "shared.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>

#include <sqlite3.h>

#include "store.h"

namespace {

const wchar_t* kRegKey = L"Software\\Inventory Manager";
constexpr auto kHeartbeatEvery = std::chrono::minutes(2);
constexpr long long kStaleAfterSeconds = 10 * 60;      // lock with no heartbeat for this long = abandoned
constexpr auto kSaveDelay = std::chrono::seconds(10);  // save this long after the last change

std::string utf8(const fs::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path from_utf8(const std::string& s) { return fs::path(reinterpret_cast<const char8_t*>(s.c_str())); }

// Registry key for the data location; automated tests use their own key (IM_REG_SUBKEY) so real settings are untouched.
std::wstring reg_key() {
    wchar_t sub[128];
    DWORD n = GetEnvironmentVariableW(L"IM_REG_SUBKEY", sub, static_cast<DWORD>(std::size(sub)));
    return std::wstring(kRegKey) + (n > 0 && n < std::size(sub) ? L"\\" + std::wstring(sub, n) : L"");
}

std::wstring reg_string(const wchar_t* name) {
    wchar_t buf[2048];
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, reg_key().c_str(), name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
    return buf;
}

void set_reg_string(HKEY key, const wchar_t* name, const std::wstring& value) {
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

long long epoch_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string file_stamp() {
    std::string s = now_iso().substr(0, 19);
    for (auto& c : s)
        if (c == 'T') c = '_';
    s.erase(std::remove(s.begin(), s.end(), ':'), s.end());
    return s;
}

std::string win_error(DWORD code) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring w = buf ? buf : L"";
    LocalFree(buf);
    while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r' || w.back() == L' ' || w.back() == L'.')) w.pop_back();
    return utf8(fs::path(w));
}

// Replaces dest with src in one step where the file system allows it.
bool replace_file(const fs::path& src, const fs::path& dest) {
    return MoveFileExW(src.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

void write_text_atomic(const fs::path& dest, const std::string& text) {
    fs::path tmp = dest;
    tmp += L".writing";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
    }
    if (!replace_file(tmp, dest)) {
        std::error_code ec;
        fs::copy_file(tmp, dest, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp, ec);
    }
}

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Read-only SQLite URI that never writes lock or journal files next to the shared file.
std::string readonly_uri(const fs::path& p) {
    std::string path = utf8(p.generic_wstring());
    std::string out = "file:";
    if (path.size() > 1 && path[1] == ':') out += "///";
    for (unsigned char c : path) {
        if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || c == ':' || c >= 0x80) out += static_cast<char>(c);
        else {
            char hex[4];
            std::snprintf(hex, sizeof hex, "%%%02X", c);
            out += hex;
        }
    }
    return out + "?mode=ro&immutable=1";
}

// Generation, who saved it and when, and item count. generation: -1 = no file, -2 = unreadable.
json read_inventory_info(const fs::path& db) {
    std::error_code ec;
    if (!fs::exists(db, ec)) return {{"generation", -1}};
    sqlite3* conn = nullptr;
    json info = {{"generation", -2}};
    if (sqlite3_open_v2(readonly_uri(db).c_str(), &conn, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) == SQLITE_OK) {
        auto query = [&](const char* sql, std::string& out) {
            sqlite3_stmt* st = nullptr;
            bool ok = false;
            if (sqlite3_prepare_v2(conn, sql, -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
                auto t = sqlite3_column_text(st, 0);
                out = t ? reinterpret_cast<const char*>(t) : "";
                ok = true;
            }
            sqlite3_finalize(st);
            return ok;
        };
        std::string items, gen, by, at;
        if (query("SELECT COUNT(*) FROM items", items)) {
            query("SELECT value FROM meta WHERE key = 'shared_generation'", gen);
            query("SELECT value FROM meta WHERE key = 'shared_saved_by'", by);
            query("SELECT value FROM meta WHERE key = 'shared_saved_at'", at);
            info = {{"generation", gen.empty() ? 0LL : std::stoll(gen)}, {"items", std::stoll(items)},
                    {"saved_by", by}, {"saved_at", at}};
        }
    }
    sqlite3_close(conn);
    return info;
}

// Checks that a copied database file is complete and is an inventory.
bool valid_inventory(const fs::path& db) {
    sqlite3* conn = nullptr;
    bool ok = false;
    if (sqlite3_open_v2(readonly_uri(db).c_str(), &conn, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) == SQLITE_OK) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(conn, "PRAGMA quick_check", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
            auto t = sqlite3_column_text(st, 0);
            ok = t && std::string(reinterpret_cast<const char*>(t)) == "ok";
        }
        sqlite3_finalize(st);
        if (ok) {
            ok = sqlite3_prepare_v2(conn, "SELECT COUNT(*) FROM items", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW;
            sqlite3_finalize(st);
        }
    }
    sqlite3_close(conn);
    return ok;
}

// Copies the shared inventory to a local file, only replacing it if the copy is complete and valid.
void pull_copy(const fs::path& src, const fs::path& dest) {
    fs::path tmp = dest;
    tmp += L".incoming";
    if (!CopyFileW(src.c_str(), tmp.c_str(), FALSE))
        throw ApiError("Couldn't read the shared inventory (" + win_error(GetLastError()) +
                       "). Make sure the folder is available and fully synced, then try again.");
    if (!valid_inventory(tmp)) {
        std::error_code ec;
        fs::remove(tmp, ec);
        throw ApiError("The shared inventory file isn't complete - it may still be syncing. "
                       "Wait for Google Drive (or your sync app) to finish, then try again.");
    }
    std::error_code ec;
    fs::path journal = dest;
    journal += L"-journal";
    fs::remove(journal, ec);
    if (!replace_file(tmp, dest)) throw ApiError("Couldn't update the local working copy (" + win_error(GetLastError()) + ")");
}

LockInfo read_lock_file(const fs::path& folder, const std::string& computer, const std::string& session) {
    LockInfo info;
    std::error_code ec;
    fs::path p = folder / "inventory.lock";
    if (!fs::exists(p, ec)) return info;
    json j = json::parse(read_text(p), nullptr, false);
    if (!j.is_object()) return info;
    info.exists = true;
    info.computer = j.value("computer", "");
    info.user = j.value("user", "");
    info.opened = j.value("opened", "");
    info.ours = !session.empty() && j.value("session", "") == session;
    info.this_computer = !computer.empty() && lower_ascii(info.computer) == lower_ascii(computer);
    long long age = epoch_seconds() - j.value("seen", 0LL);
    info.minutes_since_seen = std::max(0LL, age / 60);
    info.stale = age > kStaleAfterSeconds;
    return info;
}

}  // namespace

// ------------------------------------------------------------------ settings & identity

DataLocation DataLocation::load() {
    DataLocation d;
    d.shared = reg_string(L"DataMode") == L"shared";
    d.folder = reg_string(L"SharedFolder");
    if (d.folder.empty()) d.shared = false;
    return d;
}

void DataLocation::save() const {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, reg_key().c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        throw ApiError("Couldn't save the data location setting");
    set_reg_string(key, L"DataMode", shared ? L"shared" : L"local");
    if (shared) set_reg_string(key, L"SharedFolder", folder.wstring());
    RegCloseKey(key);
}

std::string computer_name() {
    // Lets automated tests pretend to be several computers on one machine.
    wchar_t buf[256];
    if (DWORD n = GetEnvironmentVariableW(L"IM_COMPUTER_NAME", buf, static_cast<DWORD>(std::size(buf))); n > 0 && n < std::size(buf))
        return utf8(fs::path(std::wstring(buf, n)));
    DWORD n = static_cast<DWORD>(std::size(buf));
    if (GetComputerNameExW(ComputerNameDnsHostname, buf, &n)) return utf8(fs::path(std::wstring(buf, n)));
    return "this computer";
}

std::string user_name() {
    wchar_t buf[256];
    DWORD n = static_cast<DWORD>(std::size(buf));
    if (GetUserNameW(buf, &n) && n > 0) return utf8(fs::path(std::wstring(buf, n - 1)));
    return "";
}

json LockInfo::to_json() const {
    return {{"exists", exists}, {"ours", ours}, {"this_computer", this_computer}, {"stale", stale},
            {"computer", computer}, {"user", user}, {"opened", opened}, {"minutes_since_seen", minutes_since_seen}};
}

void write_sync_state(const fs::path& cache_dir, const fs::path& folder, long long base, bool pending) {
    std::error_code ec;
    fs::create_directories(cache_dir, ec);
    write_text_atomic(cache_dir / "sync.json", json{{"folder", utf8(folder)}, {"base", base}, {"pending", pending}}.dump(2));
}

long long publish_inventory(Store& store, const fs::path& folder, const fs::path& scratch_dir, const std::string& computer) {
    long long next = std::max(file_generation(folder / "inventory.db"), 0LL) + 1;
    store.set_meta("shared_generation", std::to_string(next));
    store.set_meta("shared_saved_by", computer);
    store.set_meta("shared_saved_at", now_iso());
    std::error_code ec;
    fs::create_directories(scratch_dir, ec);
    fs::path upload = scratch_dir / "upload.db";
    fs::remove(upload, ec);
    store.backup_to(utf8(upload));
    fs::path tmp = folder / ".inventory.db.uploading";
    bool ok = CopyFileW(upload.c_str(), tmp.c_str(), FALSE) && replace_file(tmp, folder / "inventory.db");
    if (!ok) ok = CopyFileW(upload.c_str(), (folder / "inventory.db").c_str(), FALSE) != 0;
    DWORD err = GetLastError();
    fs::remove(tmp, ec);
    fs::remove(upload, ec);
    if (!ok) throw ApiError("Couldn't save the inventory into that folder (" + win_error(err) + ")");
    return next;
}

long long file_generation(const fs::path& db) { return read_inventory_info(db).value("generation", -2LL); }

json inspect_folder(const fs::path& folder) {
    std::error_code ec;
    json out = {{"path", utf8(folder)}, {"exists", fs::is_directory(folder, ec)}};
    if (!out["exists"]) return out;
    // Can we write here?
    fs::path probe = folder / (".im-write-test-" + std::to_string(epoch_seconds()));
    {
        std::ofstream f(probe);
        f << "x";
        out["writable"] = static_cast<bool>(f);
    }
    fs::remove(probe, ec);
    json info = read_inventory_info(folder / "inventory.db");
    long long gen = info.value("generation", -1LL);
    out["has_inventory"] = gen >= 0;
    out["unreadable"] = gen == -2;
    if (gen >= 0) {
        out["items"] = info.value("items", 0LL);
        out["saved_by"] = info.value("saved_by", "");
        out["saved_at"] = info.value("saved_at", "");
    }
    out["lock"] = read_lock_file(folder, computer_name(), "").to_json();
    std::string lower = lower_ascii(utf8(folder));
    out["provider"] = lower.find("google drive") != std::string::npos || lower.find("my drive") != std::string::npos ? "Google Drive"
                    : lower.find("onedrive") != std::string::npos                                                   ? "OneDrive"
                    : lower.find("dropbox") != std::string::npos                                                    ? "Dropbox"
                                                                                                                    : "";
    return out;
}

// ------------------------------------------------------------------ SharedSync

SharedSync::SharedSync(fs::path folder, fs::path cache_dir, std::string computer)
    : folder_(std::move(folder)), cache_dir_(std::move(cache_dir)), computer_(std::move(computer)), user_(user_name()) {
    std::random_device rd;
    session_ = std::to_string(rd()) + "-" + std::to_string(rd()) + "-" + std::to_string(epoch_seconds());
}

fs::path SharedSync::working_db() const { return cache_dir_ / (read_only_ ? "view.db" : "inventory.db"); }

LockInfo SharedSync::read_lock() const { return read_lock_file(folder_, computer_, session_); }

void SharedSync::load_state() {
    json j = json::parse(read_text(cache_dir_ / "sync.json"), nullptr, false);
    if (j.is_object() && lower_ascii(j.value("folder", "")) == lower_ascii(utf8(folder_))) {
        base_ = j.value("base", -1LL);
        pending_ = j.value("pending", false);
    } else {
        base_ = -1;
        pending_ = false;
    }
}

void SharedSync::save_state() {
    write_text_atomic(cache_dir_ / "sync.json", json{{"folder", utf8(folder_)}, {"base", base_}, {"pending", pending_}}.dump(2));
}

std::string SharedSync::prepare(bool read_only, const fs::path& backups_dir) {
    std::lock_guard lock(mutex_);
    read_only_ = read_only;
    std::error_code ec;
    if (!fs::is_directory(folder_, ec))
        throw ApiError("The shared folder isn't available:\n" + utf8(folder_) +
                       "\n\nMake sure Google Drive (or your sync app) is running and signed in.");
    fs::create_directories(cache_dir_, ec);
    load_state();
    long long gen = file_generation(shared_db());
    if (gen == -2)
        throw ApiError("The shared inventory file can't be read - it may still be downloading. "
                       "Wait for Google Drive (or your sync app) to finish syncing, then try again.");

    if (read_only) {
        if (gen < 0) throw ApiError("There's no shared inventory in this folder yet.");
        pull_copy(shared_db(), working_db());
        return "";
    }

    fs::path work = cache_dir_ / "inventory.db";
    bool have_work = fs::exists(work, ec);
    if (gen < 0) {
        // Nothing in the shared folder yet: this computer's working copy becomes the shared inventory.
        pending_ = true;
        save_state();
        return "";
    }
    std::string note;
    if (have_work && pending_) {
        if (base_ == gen) return "";  // our unsaved changes are the newest; they're saved as soon as the app opens
        // Another computer saved after our last save - keep our unsaved work as a backup rather than lose it.
        fs::create_directories(backups_dir, ec);
        std::string name = "unsaved-changes-" + file_stamp() + ".db";
        fs::copy_file(work, backups_dir / name, fs::copy_options::overwrite_existing, ec);
        note = "Some changes made on this computer hadn't been saved to the shared folder before another computer saved "
               "newer ones. They were kept as \"" + name + "\" in the Backups folder (File > Restore from backup).";
    }
    if (!have_work || base_ != gen || !note.empty()) {
        pull_copy(shared_db(), work);
        base_ = gen;
    }
    pending_ = false;
    save_state();
    return note;
}

void SharedSync::write_lock() {
    json j = {{"computer", computer_}, {"user", user_}, {"session", session_}, {"seen", epoch_seconds()},
              {"opened", opened_}};
    write_text_atomic(folder_ / "inventory.lock", j.dump(2));
}

void SharedSync::take_lock() {
    std::lock_guard lock(mutex_);
    opened_ = now_iso();
    write_lock();
    last_heartbeat_ = std::chrono::steady_clock::now();
}

void SharedSync::release_lock() {
    std::lock_guard lock(mutex_);
    if (read_only_) return;
    if (read_lock().ours) {
        std::error_code ec;
        fs::remove(folder_ / "inventory.lock", ec);
    }
}

void SharedSync::attach(Store& store) {
    std::lock_guard lock(mutex_);
    store_ = &store;
    clean_changes_ = seen_changes_ = store.change_count();
}

void SharedSync::note_change() {
    std::lock_guard lock(mutex_);
    if (read_only_ || closed_) return;
    last_change_ = std::chrono::steady_clock::now();
    if (!pending_) {
        pending_ = true;
        save_state();
    }
}

void SharedSync::shutdown(Store& store) {
    std::lock_guard lock(mutex_);
    if (closed_) return;
    if (!read_only_) {
        push(store);
        release_lock();
    }
    closed_ = true;
    read_only_ = true;
}

void SharedSync::tick(Store& store) {
    std::lock_guard lock(mutex_);
    if (read_only_) return;
    auto now = std::chrono::steady_clock::now();
    if (clean_changes_ < 0) clean_changes_ = store.change_count();

    if (now - last_heartbeat_ >= kHeartbeatEvery) {
        LockInfo l = read_lock();
        if (l.exists && !l.ours) {
            conflict_ = l.computer + " opened the shared inventory while this computer was using it. To avoid "
                        "overwriting each other's work, this computer has stopped saving to the shared folder. Close "
                        "Inventory Manager here and reopen it to load the latest version.";
            read_only_ = true;
            return;
        }
        std::error_code ec;
        if (fs::is_directory(folder_, ec)) write_lock();
        last_heartbeat_ = now;
    }

    long long cc = store.change_count();
    if (cc != clean_changes_) {
        if (!pending_) {
            pending_ = true;
            save_state();
            last_change_ = now;
        }
        if (cc != seen_changes_) {
            seen_changes_ = cc;
            last_change_ = now;
        }
    }
    if (pending_ && now - last_change_ >= kSaveDelay) push(store);
}

bool SharedSync::push(Store& store, bool force) {
    std::lock_guard lock(mutex_);
    if (read_only_ || !conflict_.empty()) return false;
    if (!force && !pending_ && store.change_count() == clean_changes_) return true;

    std::error_code ec;
    if (!fs::is_directory(folder_, ec)) {
        problem_ = "The shared folder isn't available right now. Changes are kept on this computer and will be saved when it's back.";
        return false;
    }
    long long gen = file_generation(shared_db());
    if (gen == -2) {
        problem_ = "The shared inventory file is busy or still syncing. Will try again shortly.";
        return false;
    }
    if (gen >= 0 && gen != base_) {
        conflict_ = "Another computer saved changes to the shared inventory while this computer had it open, so this "
                    "computer has stopped saving to avoid overwriting them. Close Inventory Manager here and reopen it: "
                    "the newest shared inventory will load, and this computer's unsaved changes will be kept in the "
                    "Backups folder.";
        read_only_ = true;
        return false;
    }
    long long next = std::max({base_, gen, 0LL}) + 1;
    store.set_meta("shared_generation", std::to_string(next));
    store.set_meta("shared_saved_by", computer_);
    store.set_meta("shared_saved_at", now_iso());

    fs::path upload = cache_dir_ / "upload.db";
    fs::remove(upload, ec);
    store.backup_to(utf8(upload));
    fs::path tmp = folder_ / ".inventory.db.uploading";
    bool ok = CopyFileW(upload.c_str(), tmp.c_str(), FALSE) && replace_file(tmp, shared_db());
    if (!ok) ok = CopyFileW(upload.c_str(), shared_db().c_str(), FALSE) != 0;
    DWORD err = GetLastError();
    fs::remove(upload, ec);
    fs::remove(tmp, ec);
    if (!ok) {
        problem_ = "Couldn't save to the shared folder (" + win_error(err) + "). Will try again shortly.";
        return false;
    }
    base_ = next;
    pending_ = false;
    save_state();
    clean_changes_ = store.change_count();
    last_saved_ = now_iso();
    problem_.clear();
    return true;
}

std::string SharedSync::read_only_reason() const {
    if (!conflict_.empty()) return conflict_;
    if (closed_) return "Inventory Manager is restarting.";
    if (read_only_)
        return "View only: the shared inventory is open on " + (lock_holder_.empty() ? std::string("another computer") : lock_holder_) +
               ". Changes can only be made on one computer at a time.";
    return "";
}

void SharedSync::set_view_only(const std::string& holder) {
    std::lock_guard lock(mutex_);
    read_only_ = true;
    lock_holder_ = holder;
}

json SharedSync::status() {
    std::lock_guard lock(mutex_);
    return {{"mode", "shared"},
            {"folder", utf8(folder_)},
            {"working_copy", utf8(working_db())},
            {"computer", computer_},
            {"read_only", read_only_.load()},
            {"read_only_reason", read_only_reason()},
            {"pending", pending_ || (store_ && !read_only_ && store_->change_count() != clean_changes_)},
            {"last_saved", last_saved_},
            {"conflict", conflict_},
            {"problem", problem_}};
}
