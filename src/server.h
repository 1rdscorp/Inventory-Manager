// Local HTTP server: the JSON API used by the app's pages, plus the pages themselves.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "util.h"

class Store;
namespace httplib { class Server; }

// Looks up a bundled page file (e.g. "index.html"); returns false if there is none.
using AssetLoader = std::function<bool(const std::string& name, std::string& body)>;

// Data location (this computer or a shared folder) is managed by the app; the server just exposes it.
struct LocationHooks {
    std::function<std::string()> read_only_reason;           // non-empty: refuse changes, with this message
    std::function<json()> status;                            // GET  /api/data-location
    std::function<json(const std::string&)> inspect;         // GET  /api/data-location/check?path=
    std::function<json(const json&)> change;                 // POST /api/data-location
    std::function<json()> sync_now;                          // POST /api/sync
    std::function<void()> changed;                           // after any successful change
};

class AppServer {
public:
    AppServer(Store& store, AssetLoader assets);
    ~AppServer();

    // Starts listening in the background. port 0 picks a free port. Returns the port, or -1.
    int start(const std::string& host, int port);
    void stop();
    void set_location_hooks(LocationHooks hooks) { hooks_ = std::move(hooks); }

private:
    void routes();

    Store& store_;
    AssetLoader assets_;
    bool loopback_ = true;
    LocationHooks hooks_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread thread_;
};
