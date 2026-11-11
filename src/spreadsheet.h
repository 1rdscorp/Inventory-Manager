// Reading .xlsx / .csv files for import, and writing .xlsx / .csv files for export.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "util.h"

struct SheetData {
    std::string sheet;                              // sheet name, or "CSV"
    std::vector<std::pair<int, Record>> records;    // (row number, fields found in that row)
};

// Finds the header row (needs "SKU" and "Item Name" columns) and returns the rows under it.
SheetData read_spreadsheet(const std::string& bytes);

std::string build_xlsx_export(const json& items, const json& movements);
std::string build_csv_export(const json& items);
