#include "server.h"

#include <cctype>
#include <filesystem>
#include <cstdio>
#include <ctime>

#include <httplib.h>
#include <shellapi.h>

#include "spreadsheet.h"
#include "store.h"

namespace {

constexpr size_t kMaxJsonBytes = 1'000'000;
constexpr size_t kMaxUploadBytes = 25u * 1024 * 1024;     // imports and datasheet files
constexpr size_t kMaxRequestBytes = 1024u * 1024 * 1024;  // restoring a backup (can hold many datasheets)
const char* kAppId = "inventory-manager";
const char* kXlsxMime = "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";

void send_json(httplib::Response& res, const json& body, int status = 200) {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content(body.dump(-1, ' ', false, json::error_handler_t::replace), "application/json; charset=utf-8");
}

json parse_json_body(const httplib::Request& req) {
    if (req.body.size() > kMaxJsonBytes) throw ApiError("That's too large to upload (limit 1 MB)", 413);
    if (req.body.empty()) return json::object();
    try {
        return json::parse(req.body);
    } catch (const json::exception&) {
        throw ApiError("Invalid JSON in request");
    }
}

Record item_record(const json& data) {
    if (!data.is_object()) throw ApiError("Invalid item data");
    Record r;
    for (const auto& f : kFields)
        if (data.contains(f)) r[f] = Value::from_json(data[f]);
    return r;
}

std::string today() {
    std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &t);
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &local);
    return buf;
}

std::string clean_filename(const std::string& raw) {
    size_t slash = raw.find_last_of("/\\");
    std::string base = slash == std::string::npos ? raw : raw.substr(slash + 1);
    std::string out;
    for (char c : base)
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    return utf8_prefix(out, 120);
}

}  // namespace

AppServer::AppServer(Store& store, AssetLoader assets)
    : store_(store), assets_(std::move(assets)), svr_(std::make_unique<httplib::Server>()) {
    routes();
}

AppServer::~AppServer() { stop(); }

int AppServer::start(const std::string& host, int port) {
    loopback_ = host == "127.0.0.1" || host == "localhost" || host == "::1";
    int bound = port == 0 ? svr_->bind_to_any_port(host) : (svr_->bind_to_port(host, port) ? port : -1);
    if (bound <= 0) return -1;
    thread_ = std::thread([this] { svr_->listen_after_bind(); });
    return bound;
}

void AppServer::stop() {
    if (svr_) svr_->stop();
    if (thread_.joinable()) thread_.join();
}

void AppServer::routes() {
    auto& s = *svr_;
    s.set_payload_max_length(kMaxRequestBytes);

    s.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        if (loopback_) {
            std::string host = lower_ascii(req.get_header_value("Host"));
            std::string name = host.rfind('[', 0) == 0 ? host.substr(0, host.find(']') + 1) : host.substr(0, host.rfind(':'));
            if (name != "localhost" && name != "127.0.0.1" && name != "[::1]") {
                send_json(res, {{"error", "Host not allowed"}}, 403);
                return httplib::Server::HandlerResponse::Handled;
            }
        }
        // Custom header => browsers block other websites from sending these requests.
        if (req.path.rfind("/api/", 0) == 0 && req.method != "GET" &&
            req.get_header_value("X-Requested-With") != kAppId) {
            send_json(res, {{"error", "Missing request header"}}, 403);
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    s.set_post_routing_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_header("X-Content-Type-Options", "nosniff");
    });

    using Handler = std::function<void(const httplib::Request&, httplib::Response&)>;
    auto safe = [](Handler fn) {
        return [fn](const httplib::Request& req, httplib::Response& res) {
            try {
                fn(req, res);
            } catch (const ApiError& e) {
                send_json(res, {{"error", e.what()}}, e.status);
            } catch (const std::exception& e) {
                send_json(res, {{"error", std::string("Unexpected error: ") + e.what()}}, 500);
            }
        };
    };

    // ---- pages
    auto page = [this](const std::string& name, const char* mime) {
        return [this, name, mime](const httplib::Request&, httplib::Response& res) {
            std::string body;
            if (!assets_(name, body)) {
                res.status = 404;
                res.set_content("Not found", "text/plain; charset=utf-8");
                return;
            }
            res.set_header("Cache-Control", "no-cache");
            res.set_content(body, mime);
        };
    };
    s.Get("/", page("index.html", "text/html; charset=utf-8"));
    s.Get("/index.html", page("index.html", "text/html; charset=utf-8"));
    s.Get("/static/app.js", page("app.js", "text/javascript; charset=utf-8"));
    s.Get("/static/styles.css", page("styles.css", "text/css; charset=utf-8"));

    // ---- API
    s.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, {{"app", kAppId}, {"ok", true}});
    });

    s.Get("/api/items", safe([this](const httplib::Request&, httplib::Response& res) {
        send_json(res, {{"items", store_.list_items()},
                        {"default_categories", kDefaultCategories},
                        {"suppliers", store_.list_suppliers()}});
    }));
    s.Post("/api/items", safe([this](const httplib::Request& req, httplib::Response& res) {
        send_json(res, {{"item", store_.create_item(item_record(parse_json_body(req)))}}, 201);
    }));
    s.Get(R"(/api/items/(\d+))", safe([this](const httplib::Request& req, httplib::Response& res) {
        send_json(res, {{"item", store_.get_item(std::stoll(req.matches[1]))}});
    }));
    s.Put(R"(/api/items/(\d+))", safe([this](const httplib::Request& req, httplib::Response& res) {
        send_json(res, {{"item", store_.update_item(std::stoll(req.matches[1]), item_record(parse_json_body(req)))}});
    }));
    s.Delete(R"(/api/items/(\d+))", safe([this](const httplib::Request& req, httplib::Response& res) {
        store_.delete_item(std::stoll(req.matches[1]));
        send_json(res, {{"ok", true}});
    }));
    s.Post(R"(/api/items/(\d+)/adjust)", safe([this](const httplib::Request& req, httplib::Response& res) {
        send_json(res, {{"item", store_.adjust(std::stoll(req.matches[1]), parse_json_body(req))}});
    }));
    s.Get(R"(/api/items/(\d+)/movements)", safe([this](const httplib::Request& req, httplib::Response& res) {
        send_json(res, {{"movements", store_.movements(500, std::stoll(req.matches[1]))}});
    }));
    // ---- attached datasheet files
    s.Post(R"(/api/items/(\d+)/datasheet)", safe([this](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size() > kMaxUploadBytes) throw ApiError("That file is too large (limit 25 MB)", 413);
        send_json(res, {{"item", store_.save_datasheet(std::stoll(req.matches[1]),
                                                       clean_filename(req.get_param_value("filename")), req.body)}});
    }));
    s.Delete(R"(/api/items/(\d+)/datasheet)", safe([this](const httplib::Request& req, httplib::Response& res) {
        send_json(res, {{"item", store_.remove_datasheet(std::stoll(req.matches[1]))}});
    }));
    s.Get(R"(/api/items/(\d+)/datasheet)", safe([this](const httplib::Request& req, httplib::Response& res) {
        std::string filename, bytes;
        if (!store_.get_datasheet(std::stoll(req.matches[1]), filename, bytes))
            throw NotFound("This item has no datasheet file attached.");
        // Only document/image types that can't run scripts are shown in place; anything else downloads.
        static const std::map<std::string, std::string> inline_types = {
            {".pdf", "application/pdf"}, {".png", "image/png"},   {".jpg", "image/jpeg"},
            {".jpeg", "image/jpeg"},     {".gif", "image/gif"},   {".webp", "image/webp"},
            {".txt", "text/plain; charset=utf-8"}};
        size_t dot = filename.find_last_of('.');
        std::string ext = dot == std::string::npos ? "" : lower_ascii(filename.substr(dot));
        auto type = inline_types.find(ext);
        // Plain-ASCII fallback name plus the exact UTF-8 name (RFC 6266).
        std::string ascii_name, encoded;
        for (unsigned char c : filename) {
            ascii_name += (c < 0x20 || c > 0x7E || c == '"' || c == '\\') ? '_' : static_cast<char>(c);
            if (isalnum(c) || c == '.' || c == '-' || c == '_') encoded += static_cast<char>(c);
            else { char hex[4]; std::snprintf(hex, sizeof hex, "%%%02X", c); encoded += hex; }
        }
        res.set_header("Cache-Control", "no-store");
        res.set_header("Content-Disposition", std::string(type != inline_types.end() ? "inline" : "attachment") +
                                                  "; filename=\"" + ascii_name + "\"; filename*=UTF-8''" + encoded);
        res.set_content(bytes, type != inline_types.end() ? type->second : "application/octet-stream");
    }));

    // ---- settings, backups, app info
    s.Get("/api/settings", safe([this](const httplib::Request&, httplib::Response& res) {
        send_json(res, store_.get_settings());
    }));
    s.Put("/api/settings", safe([this](const httplib::Request& req, httplib::Response& res) {
        send_json(res, store_.save_settings(parse_json_body(req)));
    }));
    s.Get("/api/info", safe([this](const httplib::Request&, httplib::Response& res) {
        send_json(res, {{"version", APP_VERSION}, {"data_file", store_.data_path()}, {"backups_folder", store_.backups_dir()}});
    }));
    s.Get("/api/backup", safe([this](const httplib::Request&, httplib::Response& res) {
        std::string stamp = now_iso().substr(0, 16);
        stamp[10] = ' ';
        stamp.erase(13, 1);  // "2026-11-16 1430"
        res.set_header("Cache-Control", "no-store");
        res.set_header("Content-Disposition", "attachment; filename=\"Inventory backup " + stamp + ".db\"");
        res.set_content(store_.backup_bytes(), "application/octet-stream");
    }));
    s.Get("/api/backups", safe([this](const httplib::Request&, httplib::Response& res) {
        send_json(res, {{"folder", store_.backups_dir()}, {"backups", store_.list_backups()}});
    }));
    s.Post("/api/restore", safe([this](const httplib::Request& req, httplib::Response& res) {
        int count = req.has_param("name") ? store_.restore_from_backups_dir(req.get_param_value("name"))
                                          : store_.restore_from_bytes(req.body);
        send_json(res, {{"ok", true}, {"items", count}});
    }));
    // Opens the data or backups folder in File Explorer - only for someone sitting at this computer.
    s.Post("/api/open-folder", safe([this](const httplib::Request& req, httplib::Response& res) {
        if (req.remote_addr != "127.0.0.1" && req.remote_addr != "::1")
            throw ApiError("Folders can only be opened on the computer running Inventory Manager", 403);
        std::string folder = req.get_param_value("which") == "backups" ? store_.backups_dir() : store_.data_path();
        if (req.get_param_value("which") == "backups") {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(folder.c_str())), ec);
        } else {
            folder = folder.substr(0, folder.find_last_of("/\\"));
        }
        int n = MultiByteToWideChar(CP_UTF8, 0, folder.data(), static_cast<int>(folder.size()), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, folder.data(), static_cast<int>(folder.size()), w.data(), n);
        ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        send_json(res, {{"ok", true}});
    }));

    s.Get("/api/movements", safe([this](const httplib::Request& req, httplib::Response& res) {
        int limit = 2000;
        try {
            if (req.has_param("limit")) limit = std::stoi(req.get_param_value("limit"));
        } catch (const std::exception&) {
        }
        send_json(res, {{"movements", store_.movements(limit)}});
    }));

    s.Post("/api/import", safe([this](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size() > kMaxUploadBytes) throw ApiError("That file is too large to import (limit 25 MB)", 413);
        std::string mode = req.has_param("mode") ? req.get_param_value("mode") : "merge";
        std::string dry = req.get_param_value("dry_run");
        std::string filename = clean_filename(req.get_param_value("filename"));
        SheetData data = read_spreadsheet(req.body);
        ImportSummary sum = store_.import_records(data.records, mode, dry == "1" || dry == "true",
                                                  filename.empty() ? "spreadsheet" : filename);
        sum.sheet = data.sheet;
        send_json(res, sum.to_json());
    }));

    s.Get("/api/export.xlsx", safe([this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_header("Content-Disposition", "attachment; filename=\"inventory-" + today() + ".xlsx\"");
        res.set_content(build_xlsx_export(store_.list_items(), store_.movements(20000), store_.get_settings().value("currency", "USD")), kXlsxMime);
    }));
    s.Get("/api/export.csv", safe([this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_header("Content-Disposition", "attachment; filename=\"inventory-" + today() + ".csv\"");
        res.set_content(build_csv_export(store_.list_items()), "text/csv; charset=utf-8");
    }));

    s.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
        if (req.path.rfind("/api/", 0) == 0 && res.body.empty()) {
            if (res.status == 413) send_json(res, {{"error", "That file is too large to upload"}}, 413);
            else if (res.status == 404) send_json(res, {{"error", "Not found"}}, 404);
        }
    });
}
