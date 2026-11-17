#include "util.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <ctime>

const std::vector<std::string> kFields = {"sku",      "name",          "category",  "location",   "supplier",
                                          "quantity", "reorder_level", "unit_cost", "unit_price", "notes",
                                          "supplier_url", "datasheet_url"};
const std::vector<std::string> kDefaultCategories = {"Electronics", "Office Supplies", "Furniture",
                                                     "Tools",       "Packaging",       "Other"};

namespace {

const std::vector<std::pair<std::string, size_t>> kTextLimits = {
    {"sku", 64}, {"name", 200}, {"category", 80}, {"location", 80}, {"supplier", 120}, {"notes", 2000},
    {"supplier_url", 500}, {"datasheet_url", 500}};

std::string label_for(const std::string& key) {
    static const std::map<std::string, std::string> labels = {
        {"sku", "SKU"},           {"name", "Item name"},          {"category", "Category"},
        {"location", "Location"}, {"supplier", "Supplier"},       {"notes", "Notes"},
        {"quantity", "Quantity"}, {"reorder_level", "Reorder level"}, {"unit_cost", "Unit cost"},
        {"unit_price", "Unit price"}, {"supplier_url", "Supplier website"}, {"datasheet_url", "Datasheet link"}};
    auto it = labels.find(key);
    return it == labels.end() ? key : it->second;
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

const Value& get(const Record& r, const std::string& key) {
    static const Value empty;
    auto it = r.find(key);
    return it == r.end() ? empty : it->second;
}

}  // namespace

Value Value::from_json(const json& j) {
    if (j.is_null()) return none();
    if (j.is_number()) return number(j.get<double>());
    if (j.is_string()) return str(j.get<std::string>());
    return other();
}

bool Value::is_blank() const {
    if (kind == Empty) return true;
    if (kind == Text) return trim(text).empty();
    return false;
}

std::string now_iso() {
    std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &t);
    std::tm copy = local;
    long offset = static_cast<long>(_mkgmtime(&copy) - t);  // seconds east of UTC
    char base[32];
    std::strftime(base, sizeof base, "%Y-%m-%dT%H:%M:%S", &local);
    char out[48];
    long a = std::labs(offset);
    std::snprintf(out, sizeof out, "%s%c%02ld:%02ld", base, offset < 0 ? '-' : '+', a / 3600, (a % 3600) / 60);
    return out;
}

double round4(double d) {
    if (!std::isfinite(d)) return d;
    double r = std::round(d * 10000.0) / 10000.0;
    return r == 0 ? 0.0 : r;  // no "-0"
}

json tidy_number(double d) {
    d = round4(d);
    if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 9e15) return static_cast<long long>(d);
    return d;
}

std::string number_text(double d) {
    if (!std::isfinite(d)) return "";
    if (d == std::floor(d) && std::fabs(d) < 9e15) return std::to_string(static_cast<long long>(d));
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof buf, d);
    return std::string(buf, res.ptr);
}

std::string lower_ascii(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && is_space(s[a])) ++a;
    while (b > a && is_space(s[b - 1])) --b;
    return s.substr(a, b - a);
}

size_t utf8_length(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

std::string utf8_prefix(const std::string& s, size_t max_chars) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (n == max_chars) return s.substr(0, i);
            ++n;
        }
    }
    return s;
}

double parse_number(const Value& v, const std::string& label) {
    double f = 0;
    switch (v.kind) {
        case Value::Empty:
            return 0.0;
        case Value::Other:
            throw ApiError(label + " must be a number");
        case Value::Number:
            f = v.num;
            break;
        case Value::Text: {
            std::string s;
            for (char c : trim(v.text))
                if (c != ',' && c != '$' && c != ' ') s += c;
            if (s.empty() || s == "-") return 0.0;
            const char* begin = s.data();
            const char* end = s.data() + s.size();
            if (*begin == '+' && end - begin > 1 && begin[1] != '-') ++begin;
            auto res = std::from_chars(begin, end, f);
            if (res.ec != std::errc() || res.ptr != end)
                throw ApiError(label + " must be a number (got \"" + utf8_prefix(trim(v.text), 30) + "\")");
            break;
        }
    }
    if (!std::isfinite(f)) throw ApiError(label + " must be a number");
    if (f < 0) throw ApiError(label + " can't be negative");
    if (f > 1e12) throw ApiError(label + " is too large");
    return round4(f);
}

std::string clean_text(const Value& v, const std::string& key) {
    std::string s;
    switch (v.kind) {
        case Value::Empty:
            return "";
        case Value::Other:
            throw ApiError(label_for(key) + " must be text");
        case Value::Number:
            s = std::isfinite(v.num) ? number_text(round4(v.num)) : "";
            break;
        case Value::Text:
            s = v.text;
            break;
    }
    std::string kept;
    for (char c : s)
        if (static_cast<unsigned char>(c) >= 0x20 || c == '\n' || c == '\t') kept += c;
    if (key == "notes") {
        s = trim(kept);
    } else {  // collapse runs of whitespace to single spaces
        s.clear();
        bool pending = false;
        for (char c : kept) {
            if (is_space(c)) {
                pending = !s.empty();
            } else {
                if (pending) s += ' ';
                pending = false;
                s += c;
            }
        }
    }
    for (const auto& [k, limit] : kTextLimits) {
        if (k == key && utf8_length(s) > limit)
            throw ApiError(label_for(key) + " is too long (max " + std::to_string(limit) + " characters)");
    }
    return s;
}

std::string clean_url(const Value& v, const std::string& key) {
    if (v.kind == Value::Other) throw ApiError(label_for(key) + " must be text");
    if (v.kind == Value::Empty) return "";
    if (v.kind != Value::Text) throw ApiError(label_for(key) + " must be a web address");
    std::string s;
    for (char c : v.text)
        if (static_cast<unsigned char>(c) >= 0x20) s += c;
    s = trim(s);
    if (s.empty()) return "";
    const std::string bad = label_for(key) + " must be a web address, like https://www.example.com";
    for (char c : s)
        if (is_space(c) || c == '<' || c == '>' || c == '"' || c == '`' || c == '\\' || c == '{' || c == '}' || c == '|' || c == '^')
            throw ApiError(bad);
    std::string lower = lower_ascii(s);
    if (lower.rfind("http://", 0) != 0 && lower.rfind("https://", 0) != 0) {
        // Anything else that looks like "scheme:..." (javascript:, file:, mailto:...) is refused.
        size_t colon = s.find(':');
        if (colon != std::string::npos) {
            bool letters = colon > 0;
            for (size_t i = 0; i < colon; ++i)
                if (!((lower[i] >= 'a' && lower[i] <= 'z') || lower[i] == '+' || lower[i] == '.' || lower[i] == '-')) letters = false;
            bool port = colon + 1 < s.size() && s[colon + 1] >= '0' && s[colon + 1] <= '9';
            if (letters && !port) throw ApiError(bad);
        }
        if (s.find('.') == std::string::npos || s[0] == '/' || s[0] == '.') throw ApiError(bad);
        s = "https://" + s;
    }
    size_t host_start = s.find("://") + 3;
    if (host_start >= s.size() || s[host_start] == '/' || s[host_start] == '.' || s[host_start] == ':')
        throw ApiError(bad);
    if (utf8_length(s) > 500) throw ApiError(label_for(key) + " is too long (max 500 characters)");
    return s;
}

Item validate_item(const Record& data) {
    Item it;
    it.sku = clean_text(get(data, "sku"), "sku");
    it.name = clean_text(get(data, "name"), "name");
    it.category = clean_text(get(data, "category"), "category");
    it.location = clean_text(get(data, "location"), "location");
    it.supplier = clean_text(get(data, "supplier"), "supplier");
    it.notes = clean_text(get(data, "notes"), "notes");
    it.supplier_url = clean_url(get(data, "supplier_url"), "supplier_url");
    it.datasheet_url = clean_url(get(data, "datasheet_url"), "datasheet_url");
    it.quantity = parse_number(get(data, "quantity"), "Quantity");
    it.reorder_level = parse_number(get(data, "reorder_level"), "Reorder level");
    it.unit_cost = parse_number(get(data, "unit_cost"), "Unit cost");
    it.unit_price = parse_number(get(data, "unit_price"), "Unit price");
    if (it.sku.empty()) throw ApiError("SKU is required");
    if (it.name.empty()) throw ApiError("Item name is required");
    return it;
}

Record item_to_record(const Item& it) {
    return {{"sku", Value::str(it.sku)},
            {"name", Value::str(it.name)},
            {"category", Value::str(it.category)},
            {"location", Value::str(it.location)},
            {"supplier", Value::str(it.supplier)},
            {"notes", Value::str(it.notes)},
            {"supplier_url", Value::str(it.supplier_url)},
            {"datasheet_url", Value::str(it.datasheet_url)},
            {"quantity", Value::number(it.quantity)},
            {"reorder_level", Value::number(it.reorder_level)},
            {"unit_cost", Value::number(it.unit_cost)},
            {"unit_price", Value::number(it.unit_price)}};
}
