// Local HTTP server: the JSON API used by the app's pages, plus the pages themselves.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <thread>

class Store;
namespace httplib { class Server; }

// Looks up a bundled page file (e.g. "index.html"); returns false if there is none.
using AssetLoader = std::function<bool(const std::string& name, std::string& body)>;

class AppServer {
public:
    AppServer(Store& store, AssetLoader assets);
    ~AppServer();

    // Starts listening in the background. port 0 picks a free port. Returns the port, or -1.
    int start(const std::string& host, int port);
    void stop();

private:
    void routes();

    Store& store_;
    AssetLoader assets_;
    bool loopback_ = true;
    std::unique_ptr<httplib::Server> svr_;
    std::thread thread_;
};
