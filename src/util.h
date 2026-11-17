// Shared types and helpers: errors, cell values, number/text cleaning, item validation.
#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

// An error that is reported to the browser as {"error": message} with an HTTP status.
struct ApiError : std::runtime_error {
    int status;
    explicit ApiError(const std::string& message, int status_code = 400)
        : std::runtime_error(message), status(status_code) {}
};

struct NotFound : ApiError {
    explicit NotFound(const std::string& m) : ApiError(m, 404) {}
};

struct Conflict : ApiError {
    explicit Conflict(const std::string& m) : ApiError(m, 409) {}
};

// A value read from a spreadsheet cell or a JSON request: nothing, a number, text,
// or something unusable (true/false, lists...).
struct Value {
    enum Kind { Empty, Number, Text, Other } kind = Empty;
    double num = 0;
    std::string text;

    static Value none() { return {}; }
    static Value number(double d) { Value v; v.kind = Number; v.num = d; return v; }
    static Value str(std::string s) { Value v; v.kind = Text; v.text = std::move(s); return v; }
    static Value other() { Value v; v.kind = Other; return v; }
    static Value from_json(const json& j);

    bool is_blank() const;  // empty, or text that is only whitespace
};

// A row from a spreadsheet or request: field name -> value, only for fields present.
using Record = std::map<std::string, Value>;

struct Item {
    std::string sku, name, category, location, supplier, notes;
    std::string supplier_url, datasheet_url;
    double quantity = 0, reorder_level = 0, unit_cost = 0, unit_price = 0;
};

extern const std::vector<std::string> kFields;            // all editable fields, in order
extern const std::vector<std::string> kDefaultCategories;

std::string now_iso();                      // local time, e.g. 2026-11-10T12:32:35-06:00
double round4(double d);
json tidy_number(double d);                 // 42.0 -> 42, 2.50001 -> 2.5
std::string number_text(double d);          // shortest text for a number: "42", "8.5"
std::string lower_ascii(std::string s);
std::string trim(const std::string& s);
size_t utf8_length(const std::string& s);
std::string utf8_prefix(const std::string& s, size_t max_chars);

double parse_number(const Value& v, const std::string& label);
std::string clean_text(const Value& v, const std::string& key);
// A web address: blank, or http(s)://... ("www.x.com" becomes "https://www.x.com").
std::string clean_url(const Value& v, const std::string& key);
Item validate_item(const Record& data);
Record item_to_record(const Item& item);
