// Sharing one inventory between computers through a synced folder (Google Drive, OneDrive, Dropbox...).
//
// A database file must never be edited directly inside a synced folder by two computers: the sync
// service can't merge changes, so edits get lost or the file gets damaged. Instead:
//   * each computer works on a local copy (the "working copy") and saves it to the shared folder
//     as one complete file a few seconds after each change, and when the app closes;
//   * while the app is open, a small "inventory.lock" note in the shared folder says which computer is
//     using it, so a second computer opens it view-only (or takes over if the first one crashed);
//   * every saved copy carries a generation number; if the shared file changed since this computer
//     last loaded it, that's a conflict - nothing is overwritten and the user is told.
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>

#include "util.h"

class Store;
namespace fs = std::filesystem;

// Where this Windows user keeps the inventory (stored in the registry, per computer).
struct DataLocation {
    bool shared = false;
    fs::path folder;  // the shared folder, when shared
    static DataLocation load();
    void save() const;
};

std::string computer_name();
// Records which shared generation a working copy matches (used when switching folders).
void write_sync_state(const fs::path& cache_dir, const fs::path& folder, long long base, bool pending);
// Saves a Store's inventory into a folder as inventory.db, as one complete file. Returns the new generation.
long long publish_inventory(Store& store, const fs::path& folder, const fs::path& scratch_dir, const std::string& computer);
std::string user_name();

struct LockInfo {
    bool exists = false;
    bool ours = false;        // written by this app session
    bool this_computer = false;
    bool stale = false;       // no sign of life for a while (crashed, or lost its connection)
    std::string computer, user, opened;
    long long minutes_since_seen = 0;
    json to_json() const;
};

// Facts about a candidate shared folder, for the Settings screen.
json inspect_folder(const fs::path& folder);
// Generation number saved inside an inventory file (-1 if missing or unreadable).
long long file_generation(const fs::path& db);

class SharedSync {
public:
    SharedSync(fs::path folder, fs::path cache_dir, std::string computer);

    const fs::path& folder() const { return folder_; }
    fs::path shared_db() const { return folder_ / "inventory.db"; }
    fs::path working_db() const;  // what the Store should open

    LockInfo read_lock() const;

    // Before the Store opens: brings the working copy up to date with the shared folder.
    // Returns a note for the user (e.g. "your unsaved changes were kept as a backup"), or "".
    std::string prepare(bool read_only, const fs::path& backups_dir);

    void take_lock();
    void release_lock();
    // Right after the Store opens: remember its change counter as "in sync".
    void attach(Store& store);
    // Called right after any change is saved, so a crash can't hide unsaved work.
    void note_change();
    // On exit or when switching location: final save, give up the lock, stop syncing.
    void shutdown(Store& store);

    // Called every few seconds: keeps the lock fresh and saves changes to the shared folder.
    void tick(Store& store);
    // Saves the working copy to the shared folder now if it changed (or always, if force).
    // Returns false if it couldn't (conflict, or the folder is unavailable).
    bool push(Store& store, bool force = false);

    bool read_only() const { return read_only_; }
    void set_view_only(const std::string& holder);  // opened view-only because holder has it
    std::string read_only_reason() const;
    json status();

private:
    void load_state();
    void save_state();
    void write_lock();

    fs::path folder_, cache_dir_;
    std::string computer_, user_, session_;
    std::atomic<bool> read_only_{false};
    bool closed_ = false;
    Store* store_ = nullptr;
    std::recursive_mutex mutex_;

    // Persisted in sync.json next to the working copy.
    long long base_ = -1;    // generation of the shared file the working copy matches
    bool pending_ = false;   // working copy has changes not yet saved to the shared folder

    long long clean_changes_ = -1;  // Store::change_count() when last in sync
    long long seen_changes_ = -1;   // Store::change_count() at the last tick
    std::chrono::steady_clock::time_point last_change_{}, last_heartbeat_{};
    std::string last_saved_, conflict_, problem_, lock_holder_, opened_;
};
