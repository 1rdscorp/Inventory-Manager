#include "spreadsheet.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>

#include <miniz.h>
#include <pugixml.hpp>

namespace {

constexpr size_t kMaxUnzippedBytes = 200u * 1024 * 1024;
const char* kNoHeaderMsg =
    "Couldn't find a header row with \"SKU\" and \"Item Name\" columns. "
    "Make sure the sheet has those column headings.";

using Row = std::pair<int, std::vector<Value>>;  // row number, cells by column

// ------------------------------------------------------------------ header matching

const std::map<std::string, std::string>& alias_lookup() {
    static const std::map<std::string, std::string> lookup = [] {
        const std::vector<std::pair<std::string, std::vector<std::string>>> aliases = {
            {"sku", {"sku", "sku #", "sku no", "item code", "item #", "item no", "item number", "product code",
                     "part number", "part no", "part #", "code"}},
            {"name", {"item name", "name", "item", "product", "product name", "item description", "description"}},
            {"category", {"category", "item category", "type", "group"}},
            {"location", {"location", "bin", "bin location", "storage location", "aisle", "shelf"}},
            {"supplier", {"supplier", "vendor", "supplier name", "vendor name"}},
            {"quantity", {"quantity", "qty", "quantity on hand", "qty on hand", "on hand", "in stock", "stock", "units"}},
            {"reorder_level", {"reorder level", "reorder point", "reorder at", "min qty", "minimum qty", "min stock",
                               "minimum stock", "min"}},
            {"unit_cost", {"unit cost", "cost", "cost price", "purchase price"}},
            {"unit_price", {"unit price", "price", "sale price", "selling price", "retail price"}},
            {"notes", {"notes", "note", "comments", "comment", "remarks"}},
            {"manufacturer", {"manufacturer", "manufacturer name", "mfr", "mfg", "brand", "make", "maker"}},
            {"manufacturer_url", {"manufacturer website", "manufacturer url", "manufacturer site", "manufacturer link",
                                  "mfr website", "brand website", "website", "web site"}},
            {"datasheet_url", {"datasheet url", "datasheet", "datasheet link", "data sheet", "data sheet url",
                               "data sheet link", "spec sheet", "specification sheet", "product page", "product url"}},
        };
        std::map<std::string, std::string> m;
        for (const auto& [field, list] : aliases)
            for (const auto& a : list) m.emplace(a, field);
        return m;
    }();
    return lookup;
}

std::string norm_header(const Value& v) {
    std::string s = v.kind == Value::Text ? v.text : v.kind == Value::Number ? number_text(v.num) : "";
    s = lower_ascii(s);
    std::string out;
    bool gap = false;
    for (char c : s) {
        if (c == '_') c = ' ';
        bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '#';
        if (keep) {
            if (gap && !out.empty()) out += ' ';
            gap = false;
            out += c;
        } else {
            gap = true;
        }
    }
    return out;
}

bool map_header(const std::vector<Value>& cells, std::map<size_t, std::string>& mapping) {
    mapping.clear();
    std::set<std::string> used;
    for (size_t i = 0; i < cells.size(); ++i) {
        auto it = alias_lookup().find(norm_header(cells[i]));
        if (it != alias_lookup().end() && !used.count(it->second)) {
            mapping[i] = it->second;
            used.insert(it->second);
        }
    }
    return used.count("sku") && used.count("name");
}

bool records_from_rows(const std::vector<Row>& rows, std::vector<std::pair<int, Record>>& out) {
    std::map<size_t, std::string> mapping;
    for (size_t pos = 0; pos < rows.size() && pos < 100; ++pos) {
        if (!map_header(rows[pos].second, mapping)) continue;
        out.clear();
        for (size_t r = pos + 1; r < rows.size(); ++r) {
            const auto& cells = rows[r].second;
            Record rec;
            bool any = false;
            for (const auto& [idx, field] : mapping) {
                Value v = idx < cells.size() ? cells[idx] : Value();
                if (!v.is_blank()) any = true;
                rec[field] = std::move(v);
            }
            if (any) out.emplace_back(rows[r].first, std::move(rec));
        }
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ xlsx reading

std::string local_name(const char* name) {
    const char* colon = std::strrchr(name, ':');
    return colon ? colon + 1 : name;
}

bool is(const pugi::xml_node& n, const char* local) { return n.type() == pugi::node_element && local_name(n.name()) == local; }

pugi::xml_node child(const pugi::xml_node& n, const char* local) {
    for (auto c : n.children())
        if (is(c, local)) return c;
    return {};
}

template <typename F>
void each_descendant(const pugi::xml_node& n, const char* local, F&& f) {
    for (auto c : n.children()) {
        if (is(c, local)) f(c);
        else each_descendant(c, local, f);
    }
}

std::string rich_text(const pugi::xml_node& el) {
    std::string s;
    for (auto c : el.children()) {
        if (is(c, "t")) s += c.text().get();
        else if (is(c, "r")) s += child(c, "t").text().get();
    }
    return s;
}

int col_index(const char* ref) {
    int n = 0;
    for (; *ref >= 'A' && *ref <= 'Z'; ++ref) n = n * 26 + (*ref - 'A' + 1);
    return n - 1;
}

Value cell_value(const pugi::xml_node& c, const std::vector<std::string>& shared) {
    std::string t = c.attribute("t") ? c.attribute("t").value() : "n";
    if (t == "inlineStr") {
        auto is_ = child(c, "is");
        return Value::str(is_ ? rich_text(is_) : "");
    }
    auto v = child(c, "v");
    if (!v || !v.first_child()) return Value();
    std::string text = v.text().get();
    if (t == "s") {
        char* end = nullptr;
        long idx = std::strtol(text.c_str(), &end, 10);
        return (end && *end == 0 && idx >= 0 && static_cast<size_t>(idx) < shared.size()) ? Value::str(shared[idx])
                                                                                          : Value::str("");
    }
    if (t == "str") return Value::str(text);
    if (t == "e") return Value();
    if (t == "b") return Value::str(text == "1" ? "TRUE" : "FALSE");
    char* end = nullptr;
    double d = std::strtod(text.c_str(), &end);
    if (end && *end == 0 && !text.empty()) return Value::number(d);
    return Value::str(text);
}

std::vector<Row> read_sheet(const pugi::xml_node& root, const std::vector<std::string>& shared) {
    std::vector<Row> rows;
    each_descendant(root, "row", [&](const pugi::xml_node& row) {
        int rownum = row.attribute("r").as_int(0);
        if (rownum <= 0) rownum = rows.empty() ? 1 : rows.back().first + 1;
        std::vector<Value> cells;
        int next_col = 0;
        for (auto c : row.children()) {
            if (!is(c, "c")) continue;
            int col = c.attribute("r") ? col_index(c.attribute("r").value()) : next_col;
            next_col = col + 1;
            if (col < 0 || col > 300) continue;
            if (static_cast<size_t>(col) >= cells.size()) cells.resize(col + 1);
            cells[col] = cell_value(c, shared);
        }
        rows.emplace_back(rownum, std::move(cells));
    });
    return rows;
}

std::string normalize_path(const std::string& path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        std::string seg = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (seg == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!seg.empty() && seg != ".") parts.push_back(seg);
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : "/") + p;
    return out;
}

class ZipReader {
public:
    explicit ZipReader(const std::string& data) {
        if (!mz_zip_reader_init_mem(&zip_, data.data(), data.size(), 0))
            throw ApiError("This file isn't a valid .xlsx workbook");
        ok_ = true;
        mz_uint n = mz_zip_reader_get_num_files(&zip_);
        unsigned long long total = 0;
        for (mz_uint i = 0; i < n; ++i) {
            mz_zip_archive_file_stat st;
            if (mz_zip_reader_file_stat(&zip_, i, &st)) {
                total += st.m_uncomp_size;
                names_[st.m_filename] = i;
            }
        }
        if (total > kMaxUnzippedBytes) throw ApiError("This workbook is too large to import");
    }
    ~ZipReader() { if (ok_) mz_zip_reader_end(&zip_); }
    bool has(const std::string& name) const { return names_.count(name) > 0; }
    std::string read(const std::string& name) {
        size_t size = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip_, names_.at(name), &size, 0);
        if (!p) throw ApiError("This workbook couldn't be read. It may be damaged.");
        std::string s(static_cast<const char*>(p), size);
        mz_free(p);
        return s;
    }

private:
    mz_zip_archive zip_{};
    bool ok_ = false;
    std::map<std::string, mz_uint> names_;
};

void parse_xml(pugi::xml_document& doc, const std::string& text) {
    if (!doc.load_buffer(text.data(), text.size(), pugi::parse_default | pugi::parse_ws_pcdata_single))
        throw ApiError("This workbook couldn't be read. It may be damaged.");
}

bool read_xlsx(const std::string& data, SheetData& out) {
    ZipReader zip(data);
    if (!zip.has("xl/workbook.xml")) throw ApiError("This file isn't an Excel workbook (.xlsx)");

    std::vector<std::string> shared;
    if (zip.has("xl/sharedStrings.xml")) {
        pugi::xml_document doc;
        parse_xml(doc, zip.read("xl/sharedStrings.xml"));
        each_descendant(doc, "si", [&](const pugi::xml_node& si) { shared.push_back(rich_text(si)); });
    }
    std::map<std::string, std::string> rels;
    if (zip.has("xl/_rels/workbook.xml.rels")) {
        pugi::xml_document doc;
        parse_xml(doc, zip.read("xl/_rels/workbook.xml.rels"));
        each_descendant(doc, "Relationship", [&](const pugi::xml_node& r) {
            rels[r.attribute("Id").value()] = r.attribute("Target").value();
        });
    }
    std::vector<std::pair<std::string, std::string>> sheets;  // name, path
    {
        pugi::xml_document doc;
        parse_xml(doc, zip.read("xl/workbook.xml"));
        each_descendant(doc, "sheet", [&](const pugi::xml_node& sh) {
            std::string rid;
            for (auto a : sh.attributes()) {
                std::string n = a.name();
                if (n.size() > 3 && n.compare(n.size() - 3, 3, ":id") == 0) rid = a.value();
            }
            std::string target = rels.count(rid) ? rels[rid] : "";
            std::string path = !target.empty() && target[0] == '/' ? target.substr(1) : normalize_path("xl/" + target);
            if (zip.has(path)) {
                std::string name = sh.attribute("name").value();
                sheets.emplace_back(name.empty() ? "Sheet" : name, path);
            }
        });
    }
    std::stable_sort(sheets.begin(), sheets.end(), [](const auto& a, const auto& b) {
        return (lower_ascii(trim(a.first)) == "inventory") > (lower_ascii(trim(b.first)) == "inventory");
    });
    for (const auto& [name, path] : sheets) {
        pugi::xml_document doc;
        parse_xml(doc, zip.read(path));
        if (records_from_rows(read_sheet(doc, shared), out.records)) {
            out.sheet = name;
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------ csv reading

bool valid_utf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0) return false;
        for (int k = 1; k <= n; ++k)
            if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += n + 1;
    }
    return true;
}

// Windows-1252 bytes 0x80-0x9F; other bytes map to the same Unicode code point (Latin-1).
std::string cp1252_to_utf8(const std::string& s) {
    static const unsigned short high[32] = {0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                            0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D,   0x017D, 0x8F,
                                            0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                            0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};
    std::string out;
    for (unsigned char c : s) {
        unsigned cp = (c >= 0x80 && c < 0xA0) ? high[c - 0x80] : c;
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

char sniff_delimiter(const std::string& text) {
    // Use the first line: whichever of , ; tab appears most (outside quotes).
    int counts[3] = {0, 0, 0};
    const char delims[3] = {',', ';', '\t'};
    bool quoted = false;
    for (size_t i = 0; i < text.size() && i < 4096; ++i) {
        char c = text[i];
        if (c == '"') quoted = !quoted;
        else if (!quoted && (c == '\n' || c == '\r')) break;
        else if (!quoted)
            for (int k = 0; k < 3; ++k)
                if (c == delims[k]) ++counts[k];
    }
    int best = 0;
    for (int k = 1; k < 3; ++k)
        if (counts[k] > counts[best]) best = k;
    return delims[best];
}

std::vector<Row> read_csv(std::string data) {
    if (data.size() >= 3 && data.compare(0, 3, "\xEF\xBB\xBF") == 0) data.erase(0, 3);
    std::string text = valid_utf8(data) ? data : cp1252_to_utf8(data);
    char delim = sniff_delimiter(text);

    std::vector<Row> rows;
    std::vector<Value> row;
    std::string field;
    bool in_quotes = false, field_started = false;
    auto end_field = [&] {
        // Undo the formula-safety apostrophe added by the app's own CSV export.
        if (field.size() >= 2 && field[0] == '\'' && (field[1] == '=' || field[1] == '+' || field[1] == '@'))
            field.erase(0, 1);
        row.push_back(Value::str(field));
        field.clear();
        field_started = false;
    };
    auto end_row = [&] {
        end_field();
        rows.emplace_back(static_cast<int>(rows.size()) + 1, std::move(row));
        row.clear();
    };
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') { field += '"'; ++i; }
                else in_quotes = false;
            } else {
                field += c;
            }
        } else if (c == '"' && !field_started) {
            in_quotes = true;
            field_started = true;
        } else if (c == delim) {
            end_field();
        } else if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            end_row();
        } else {
            field += c;
            field_started = true;
        }
    }
    if (field_started || !field.empty() || !row.empty()) end_row();
    return rows;
}

// ------------------------------------------------------------------ export

struct Column {
    const char* header;
    const char* key;
    int width;
    const char* kind;     // "text" | "num" | "money"
    const char* formula;  // xlsx only; "{r}" is the row number
};

const std::vector<Column> kItemColumns = {
    {"SKU", "sku", 14, "text", nullptr},
    {"Item Name", "name", 30, "text", nullptr},
    {"Category", "category", 16, "text", nullptr},
    {"Location", "location", 14, "text", nullptr},
    {"Supplier", "supplier", 24, "text", nullptr},
    {"Quantity", "quantity", 11, "num", nullptr},
    {"Reorder Level", "reorder_level", 14, "num", nullptr},
    {"Unit Cost", "unit_cost", 12, "money", nullptr},
    {"Unit Price", "unit_price", 12, "money", nullptr},
    {"Total Value", "total_value", 14, "money", "F{r}*H{r}"},
    {"Status", "status", 11, "text", "IF(F{r}<=G{r},\"Reorder\",\"OK\")"},
    {"Notes", "notes", 40, "text", nullptr},
    {"Manufacturer", "manufacturer", 20, "text", nullptr},
    {"Manufacturer Website", "manufacturer_url", 34, "url", nullptr},
    {"Datasheet URL", "datasheet_url", 40, "url", nullptr},
    {"Datasheet File", "datasheet_file", 24, "text", nullptr},
};
const std::vector<Column> kMovementColumns = {
    {"When", "created_at", 22, "text", nullptr},
    {"SKU", "sku", 14, "text", nullptr},
    {"Item Name", "item_name", 30, "text", nullptr},
    {"Type", "kind", 12, "text", nullptr},
    {"Change", "change", 10, "num", nullptr},
    {"On Hand After", "quantity_after", 14, "num", nullptr},
    {"Note", "note", 40, "text", nullptr},
};

const char* kNsMain = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
const char* kNsRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";

std::string col_letter(int i) {
    std::string s;
    for (++i; i; i = (i - 1) / 26) s.insert(s.begin(), static_cast<char>('A' + (i - 1) % 26));
    return s;
}

std::string xml_text(const std::string& in) {
    std::string out;
    for (char c : in) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 && c != '\t' && c != '\n' && c != '\r') continue;
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string json_number_text(const json& v) {
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    return number_text(v.get<double>());
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) s.replace(p, from.size(), to);
    return s;
}

// links: (cell, web address) pairs for clickable "url" cells, written as sheet hyperlinks.
std::string sheet_xml(const std::vector<Column>& cols, const json& records, std::string& last_ref,
                      std::vector<std::pair<std::string, std::string>>& links) {
    std::string x = std::string(R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><worksheet xmlns=")") +
                    kNsMain + "\" xmlns:r=\"" + kNsRel +
                    "\"><sheetViews><sheetView workbookViewId=\"0\"><pane ySplit=\"1\" topLeftCell=\"A2\" "
                    "activePane=\"bottomLeft\" state=\"frozen\"/></sheetView></sheetViews>"
                    "<sheetFormatPr defaultRowHeight=\"15\"/><cols>";
    for (size_t i = 0; i < cols.size(); ++i)
        x += "<col min=\"" + std::to_string(i + 1) + "\" max=\"" + std::to_string(i + 1) + "\" width=\"" +
             std::to_string(cols[i].width) + "\" customWidth=\"1\"/>";
    x += "</cols><sheetData><row r=\"1\">";
    for (size_t i = 0; i < cols.size(); ++i)
        x += "<c r=\"" + col_letter(static_cast<int>(i)) + "1\" t=\"inlineStr\" s=\"1\"><is><t>" + xml_text(cols[i].header) +
             "</t></is></c>";
    x += "</row>";
    int r = 2;
    for (const auto& rec : records) {
        std::string rs = std::to_string(r);
        x += "<row r=\"" + rs + "\">";
        for (size_t i = 0; i < cols.size(); ++i) {
            const Column& col = cols[i];
            std::string ref = col_letter(static_cast<int>(i)) + rs;
            json value = rec.contains(col.key) ? rec[col.key] : json();
            std::string f = col.formula ? "<f>" + xml_text(replace_all(col.formula, "{r}", rs)) + "</f>" : "";
            std::string kind = col.kind;
            std::string style = kind == "money" ? " s=\"2\"" : kind == "url" ? " s=\"3\"" : "";
            if (value.is_number()) {
                x += "<c r=\"" + ref + "\"" + style + ">" + f + "<v>" + json_number_text(value) + "</v></c>";
            } else if (value.is_string() && !value.get<std::string>().empty()) {
                std::string t = xml_text(value.get<std::string>());
                if (col.formula) x += "<c r=\"" + ref + "\" t=\"str\">" + f + "<v>" + t + "</v></c>";
                else x += "<c r=\"" + ref + "\"" + style + " t=\"inlineStr\"><is><t xml:space=\"preserve\">" + t + "</t></is></c>";
                if (kind == "url") links.emplace_back(ref, value.get<std::string>());
            }
        }
        x += "</row>";
        ++r;
    }
    last_ref = col_letter(static_cast<int>(cols.size()) - 1) + std::to_string(records.size() + 1);
    x += "</sheetData><autoFilter ref=\"A1:" + last_ref + "\"/>";
    if (!links.empty()) {
        x += "<hyperlinks>";
        for (size_t i = 0; i < links.size(); ++i)
            x += "<hyperlink ref=\"" + links[i].first + "\" r:id=\"rIdLink" + std::to_string(i + 1) + "\"/>";
        x += "</hyperlinks>";
    }
    x += "</worksheet>";
    return x;
}

const char* kStylesXml =
    R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">)"
    R"(<numFmts count="1"><numFmt numFmtId="164" formatCode="\$#,##0.00"/></numFmts>)"
    R"(<fonts count="3"><font><sz val="11"/><name val="Calibri"/></font>)"
    R"(<font><b/><sz val="11"/><color rgb="FFFFFFFF"/><name val="Calibri"/></font>)"
    R"(<font><u/><sz val="11"/><color rgb="FF0563C1"/><name val="Calibri"/></font></fonts>)"
    R"(<fills count="3"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill>)"
    R"(<fill><patternFill patternType="solid"><fgColor rgb="FF1F4E79"/><bgColor indexed="64"/></patternFill></fill></fills>)"
    R"(<borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>)"
    R"(<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>)"
    R"(<cellXfs count="4"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/>)"
    R"(<xf numFmtId="0" fontId="1" fillId="2" borderId="0" xfId="0" applyFont="1" applyFill="1"/>)"
    R"(<xf numFmtId="164" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>)"
    R"(<xf numFmtId="0" fontId="2" fillId="0" borderId="0" xfId="0" applyFont="1"/></cellXfs>)"
    R"(<cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles></styleSheet>)";

class ZipWriter {
public:
    ZipWriter() {
        if (!mz_zip_writer_init_heap(&zip_, 0, 0)) throw std::runtime_error("Couldn't create the workbook");
    }
    ~ZipWriter() { mz_zip_writer_end(&zip_); }
    void add(const std::string& name, const std::string& data) {
        if (!mz_zip_writer_add_mem(&zip_, name.c_str(), data.data(), data.size(), MZ_DEFAULT_LEVEL))
            throw std::runtime_error("Couldn't create the workbook");
    }
    std::string finish() {
        void* buf = nullptr;
        size_t size = 0;
        if (!mz_zip_writer_finalize_heap_archive(&zip_, &buf, &size))
            throw std::runtime_error("Couldn't create the workbook");
        std::string out(static_cast<const char*>(buf), size);
        mz_free(buf);
        return out;
    }

private:
    mz_zip_archive zip_{};
};

struct SheetSpec {
    std::string name;
    const std::vector<Column>* cols;
    json records;
};

std::string build_xlsx(const std::vector<SheetSpec>& sheets) {
    const std::string decl = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)";
    const std::string pkg_rels = "http://schemas.openxmlformats.org/package/2006/relationships";
    std::vector<std::string> parts, sheet_rels;
    std::string defined;
    for (size_t i = 0; i < sheets.size(); ++i) {
        std::string last;
        std::vector<std::pair<std::string, std::string>> links;
        parts.push_back(sheet_xml(*sheets[i].cols, sheets[i].records, last, links));
        std::string rels;
        if (!links.empty()) {
            rels = decl + "<Relationships xmlns=\"" + pkg_rels + "\">";
            for (size_t k = 0; k < links.size(); ++k)
                rels += "<Relationship Id=\"rIdLink" + std::to_string(k + 1) + "\" Type=\"" + kNsRel +
                        "/hyperlink\" Target=\"" + xml_text(links[k].second) + "\" TargetMode=\"External\"/>";
            rels += "</Relationships>";
        }
        sheet_rels.push_back(rels);
        size_t digit = last.find_first_of("0123456789");
        defined += "<definedName name=\"_xlnm._FilterDatabase\" localSheetId=\"" + std::to_string(i) +
                   "\" hidden=\"1\">'" + xml_text(sheets[i].name) + "'!$A$1:$" + last.substr(0, digit) + "$" +
                   last.substr(digit) + "</definedName>";
    }
    std::string types = decl + "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>";
    std::string sheet_list, wb_rels = decl + "<Relationships xmlns=\"" + pkg_rels + "\">";
    for (size_t i = 0; i < sheets.size(); ++i) {
        std::string n = std::to_string(i + 1);
        types += "<Override PartName=\"/xl/worksheets/sheet" + n +
                 ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>";
        sheet_list += "<sheet name=\"" + xml_text(sheets[i].name) + "\" sheetId=\"" + n + "\" r:id=\"rId" + n + "\"/>";
        wb_rels += "<Relationship Id=\"rId" + n + "\" Type=\"" + kNsRel + "/worksheet\" Target=\"worksheets/sheet" + n + ".xml\"/>";
    }
    types += "</Types>";
    wb_rels += "<Relationship Id=\"rId" + std::to_string(sheets.size() + 1) + "\" Type=\"" + kNsRel +
               "/styles\" Target=\"styles.xml\"/></Relationships>";
    std::string root_rels = decl + "<Relationships xmlns=\"" + pkg_rels + "\"><Relationship Id=\"rId1\" Type=\"" + kNsRel +
                            "/officeDocument\" Target=\"xl/workbook.xml\"/></Relationships>";
    std::string workbook = decl + "<workbook xmlns=\"" + kNsMain + "\" xmlns:r=\"" + kNsRel + "\"><sheets>" + sheet_list +
                           "</sheets><definedNames>" + defined + "</definedNames><calcPr calcId=\"191029\" fullCalcOnLoad=\"1\"/></workbook>";

    ZipWriter z;
    z.add("[Content_Types].xml", types);
    z.add("_rels/.rels", root_rels);
    z.add("xl/workbook.xml", workbook);
    z.add("xl/_rels/workbook.xml.rels", wb_rels);
    z.add("xl/styles.xml", kStylesXml);
    for (size_t i = 0; i < parts.size(); ++i) {
        z.add("xl/worksheets/sheet" + std::to_string(i + 1) + ".xml", parts[i]);
        if (!sheet_rels[i].empty())
            z.add("xl/worksheets/_rels/sheet" + std::to_string(i + 1) + ".xml.rels", sheet_rels[i]);
    }
    return z.finish();
}

}  // namespace

SheetData read_spreadsheet(const std::string& bytes) {
    if (bytes.empty()) throw ApiError("The file is empty");
    if (bytes.size() >= 4 && bytes.compare(0, 4, "\xD0\xCF\x11\xE0") == 0)
        throw ApiError("Old .xls files aren't supported. Open the file in Excel and use "
                       "Save As > Excel Workbook (.xlsx), then import that.");
    SheetData out;
    if (bytes.compare(0, 2, "PK") == 0) {
        if (!read_xlsx(bytes, out)) throw ApiError(kNoHeaderMsg);
        return out;
    }
    if (!records_from_rows(read_csv(bytes), out.records)) throw ApiError(kNoHeaderMsg);
    out.sheet = "CSV";
    return out;
}

std::string build_xlsx_export(const json& items, const json& movements) {
    static const std::map<std::string, std::string> kinds = {
        {"created", "Created"}, {"received", "Received"}, {"removed", "Removed"}, {"counted", "Count"},
        {"edited", "Edited"},   {"imported", "Imported"}, {"deleted", "Deleted"}};
    json activity = json::array();
    for (auto m : movements) {
        std::string k = m["kind"].get<std::string>();
        if (kinds.count(k)) m["kind"] = kinds.at(k);
        std::string when = m["created_at"].get<std::string>();
        std::replace(when.begin(), when.end(), 'T', ' ');
        m["created_at"] = when.substr(0, 19);
        activity.push_back(std::move(m));
    }
    return build_xlsx({{"Inventory", &kItemColumns, items}, {"Activity", &kMovementColumns, activity}});
}

std::string build_csv_export(const json& items) {
    auto quote = [](const std::string& s) {
        if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
        std::string q = "\"";
        for (char c : s) q += c == '"' ? std::string("\"\"") : std::string(1, c);
        return q + "\"";
    };
    std::string out = "\xEF\xBB\xBF";
    for (size_t i = 0; i < kItemColumns.size(); ++i) out += (i ? "," : "") + quote(kItemColumns[i].header);
    out += "\r\n";
    for (const auto& it : items) {
        for (size_t i = 0; i < kItemColumns.size(); ++i) {
            const json& v = it[kItemColumns[i].key];
            std::string s;
            if (v.is_number()) {
                s = json_number_text(v);
            } else if (v.is_string()) {
                s = v.get<std::string>();
                // Stop spreadsheet apps from treating text like "=..." as a formula.
                if (!s.empty() && (s[0] == '=' || s[0] == '+' || s[0] == '@')) s = "'" + s;
            }
            out += (i ? "," : "") + quote(s);
        }
        out += "\r\n";
    }
    return out;
}
