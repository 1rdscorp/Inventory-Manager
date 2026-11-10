#!/usr/bin/env python3
"""
Inventory Manager - a searchable inventory app that runs on your own computer.

Start it with:   python server.py        (or double-click "Start Inventory Manager.bat")
It opens:        http://127.0.0.1:8765/

Only the Python standard library is used - nothing to install.
Data lives in data/inventory.db (SQLite). On first run the app imports
inventory_management.xlsx from this folder if it is present.
"""

import argparse
import csv
import io
import json
import math
import os
import posixpath
import re
import sqlite3
import sys
import threading
import traceback
import urllib.request
import webbrowser
import xml.etree.ElementTree as ET
import zipfile
from contextlib import contextmanager
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlparse

APP_ID = "inventory-manager"
FROZEN = getattr(sys, "frozen", False)  # True when running as "Inventory Manager.exe"
# The exe is built without a terminal window; it then has no console to print to and
# runs from an icon in the Windows system tray instead (next to the clock).
NO_CONSOLE = sys.stdout is None
if FROZEN:
    # Data sits next to the .exe; the web page files are unpacked into a temp folder.
    APP_DIR = os.path.dirname(os.path.abspath(sys.executable))
    RESOURCE_DIR = getattr(sys, "_MEIPASS", APP_DIR)
else:
    APP_DIR = RESOURCE_DIR = os.path.dirname(os.path.abspath(__file__))
STATIC_DIR = os.path.join(RESOURCE_DIR, "static")
DEFAULT_DB = os.path.join(APP_DIR, "data", "inventory.db")
SEED_FILE = os.path.join(APP_DIR, "inventory_management.xlsx")

DEFAULT_CATEGORIES = ["Electronics", "Office Supplies", "Furniture", "Tools", "Packaging", "Other"]
FIELDS = ["sku", "name", "category", "location", "supplier",
          "quantity", "reorder_level", "unit_cost", "unit_price", "notes"]
TEXT_LIMITS = {"sku": 64, "name": 200, "category": 80, "location": 80, "supplier": 120, "notes": 2000}
NUMBER_FIELDS = {"quantity": "Quantity", "reorder_level": "Reorder level",
                 "unit_cost": "Unit cost", "unit_price": "Unit price"}
LABELS = {"sku": "SKU", "name": "Item name", "category": "Category", "location": "Location",
          "supplier": "Supplier", "notes": "Notes", **NUMBER_FIELDS}

MAX_JSON_BYTES = 1_000_000
MAX_UPLOAD_BYTES = 25 * 1024 * 1024
MAX_UNZIPPED_BYTES = 200 * 1024 * 1024


# --------------------------------------------------------------------------- errors

class ApiError(Exception):
    status = 400

    def __init__(self, message, status=None):
        super().__init__(message)
        if status:
            self.status = status


class NotFound(ApiError):
    status = 404


class Conflict(ApiError):
    status = 409


class _DryRun(Exception):
    """Raised inside a transaction to roll back an import preview."""


# --------------------------------------------------------------------------- value helpers

def now_iso():
    return datetime.now().astimezone().isoformat(timespec="seconds")


def tidy_number(value):
    """Whole numbers become ints so JSON/CSV show 42 rather than 42.0."""
    f = float(value or 0)
    return int(f) if f.is_integer() else round(f, 4)


def parse_number(value, label):
    if value is None:
        return 0.0
    if isinstance(value, bool):
        raise ApiError(f"{label} must be a number")
    if isinstance(value, (int, float)):
        f = float(value)
    else:
        s = str(value).strip().replace(",", "").replace("$", "").replace(" ", "")
        if s in ("", "-"):
            return 0.0
        try:
            f = float(s)
        except ValueError:
            raise ApiError(f'{label} must be a number (got "{str(value).strip()[:30]}")')
    if not math.isfinite(f):
        raise ApiError(f"{label} must be a number")
    if f < 0:
        raise ApiError(f"{label} can't be negative")
    if f > 1e12:
        raise ApiError(f"{label} is too large")
    return round(f, 4)


def clean_text(value, key):
    if value is None:
        return ""
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        value = str(tidy_number(value)) if math.isfinite(value) else ""
    if not isinstance(value, str):
        raise ApiError(f"{LABELS[key]} must be text")
    value = "".join(ch for ch in value if ch >= " " or ch in "\n\t")
    value = value.strip() if key == "notes" else " ".join(value.split())
    if len(value) > TEXT_LIMITS[key]:
        raise ApiError(f"{LABELS[key]} is too long (max {TEXT_LIMITS[key]} characters)")
    return value


def validate_item(data):
    if not isinstance(data, dict):
        raise ApiError("Invalid item data")
    item = {key: clean_text(data.get(key), key) for key in TEXT_LIMITS}
    for key, label in NUMBER_FIELDS.items():
        item[key] = parse_number(data.get(key), label)
    if not item["sku"]:
        raise ApiError("SKU is required")
    if not item["name"]:
        raise ApiError("Item name is required")
    return item


def _same(a, b):
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a - b) < 1e-9
    return a == b


def item_to_dict(row):
    qty, level, cost = row["quantity"], row["reorder_level"], row["unit_cost"]
    return {
        "id": row["id"],
        "sku": row["sku"],
        "name": row["name"],
        "category": row["category"],
        "location": row["location"],
        "supplier": row["supplier"],
        "quantity": tidy_number(qty),
        "reorder_level": tidy_number(level),
        "unit_cost": tidy_number(cost),
        "unit_price": tidy_number(row["unit_price"]),
        "notes": row["notes"],
        # Same rules as the spreadsheet: Total Value = Quantity x Unit Cost,
        # Status = "Reorder" when Quantity <= Reorder Level.
        "total_value": round(qty * cost, 2),
        "status": "Reorder" if qty <= level else "OK",
        "created_at": row["created_at"],
        "updated_at": row["updated_at"],
    }


def movement_to_dict(row):
    return {
        "id": row["id"],
        "item_id": row["item_id"],
        "sku": row["sku"],
        "item_name": row["item_name"],
        "change": tidy_number(row["change"]),
        "quantity_after": tidy_number(row["quantity_after"]),
        "kind": row["kind"],
        "note": row["note"],
        "created_at": row["created_at"],
    }


# --------------------------------------------------------------------------- database

SCHEMA = """
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
"""

INSERT_ITEM = ("INSERT INTO items (sku, name, category, location, supplier, quantity, reorder_level, "
               "unit_cost, unit_price, notes, created_at, updated_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)")
UPDATE_ITEM = ("UPDATE items SET sku=?, name=?, category=?, location=?, supplier=?, quantity=?, "
               "reorder_level=?, unit_cost=?, unit_price=?, notes=?, updated_at=? WHERE id=?")


class Store:
    def __init__(self, path):
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        self.path = path
        self.lock = threading.RLock()
        self.conn = sqlite3.connect(path, check_same_thread=False, isolation_level=None)
        self.conn.row_factory = sqlite3.Row
        self.conn.executescript(SCHEMA)

    def close(self):
        with self.lock:
            self.conn.close()

    @contextmanager
    def transaction(self):
        with self.lock:
            self.conn.execute("BEGIN IMMEDIATE")
            try:
                yield self.conn
            except BaseException:
                self.conn.execute("ROLLBACK")
                raise
            else:
                self.conn.execute("COMMIT")

    def _read(self, sql, params=()):
        with self.lock:
            return self.conn.execute(sql, params).fetchall()

    # ---- internal helpers (call inside a transaction)

    @staticmethod
    def _fetch(conn, item_id):
        row = conn.execute("SELECT * FROM items WHERE id = ?", (item_id,)).fetchone()
        if row is None:
            raise NotFound("That item no longer exists. It may have been deleted.")
        return row

    @staticmethod
    def _check_sku_free(conn, sku, exclude_id=None):
        row = conn.execute("SELECT name FROM items WHERE sku = ? AND id IS NOT ?", (sku, exclude_id)).fetchone()
        if row:
            raise Conflict(f'SKU "{sku}" is already used by "{row["name"]}"')

    @staticmethod
    def _log(conn, item_id, sku, name, change, after, kind, note=""):
        conn.execute(
            "INSERT INTO movements (item_id, sku, item_name, change, quantity_after, kind, note, created_at) "
            "VALUES (?,?,?,?,?,?,?,?)",
            (item_id, sku, name, round(change, 4), round(after, 4), kind, note, now_iso()))

    def _insert(self, conn, item, kind="created", note=""):
        ts = now_iso()
        cur = conn.execute(INSERT_ITEM, (*[item[k] for k in FIELDS], ts, ts))
        self._log(conn, cur.lastrowid, item["sku"], item["name"], item["quantity"], item["quantity"], kind, note)
        return cur.lastrowid

    def _update(self, conn, row, item, kind="edited", note=""):
        conn.execute(UPDATE_ITEM, (*[item[k] for k in FIELDS], now_iso(), row["id"]))
        change = item["quantity"] - row["quantity"]
        if abs(change) > 1e-9:
            self._log(conn, row["id"], item["sku"], item["name"], change, item["quantity"], kind, note)

    # ---- public API

    def list_items(self):
        return [item_to_dict(r) for r in self._read("SELECT * FROM items ORDER BY sku COLLATE NOCASE")]

    def get_item(self, item_id):
        with self.lock:
            return item_to_dict(self._fetch(self.conn, item_id))

    def create_item(self, data):
        item = validate_item(data)
        with self.transaction() as conn:
            self._check_sku_free(conn, item["sku"])
            new_id = self._insert(conn, item)
        return self.get_item(new_id)

    def update_item(self, item_id, data):
        if not isinstance(data, dict):
            raise ApiError("Invalid item data")
        with self.transaction() as conn:
            row = self._fetch(conn, item_id)
            merged = {k: row[k] for k in FIELDS}
            merged.update({k: v for k, v in data.items() if k in FIELDS})
            item = validate_item(merged)
            self._check_sku_free(conn, item["sku"], item_id)
            self._update(conn, row, item, "edited", "Quantity changed in item details")
        return self.get_item(item_id)

    def delete_item(self, item_id):
        with self.transaction() as conn:
            row = self._fetch(conn, item_id)
            conn.execute("DELETE FROM items WHERE id = ?", (item_id,))
            self._log(conn, item_id, row["sku"], row["name"], -row["quantity"], 0, "deleted", "Item deleted")

    def adjust(self, item_id, data):
        if not isinstance(data, dict):
            raise ApiError("Invalid request")
        if data.get("amount") in (None, ""):
            raise ApiError("Enter a quantity")
        mode = data.get("mode")
        amount = parse_number(data.get("amount"), "Quantity")
        note = clean_text(data.get("note"), "notes")[:300]
        with self.transaction() as conn:
            row = self._fetch(conn, item_id)
            current = row["quantity"]
            if mode == "add":
                if amount <= 0:
                    raise ApiError("Enter how many units you received")
                new_qty, kind = current + amount, "received"
            elif mode == "remove":
                if amount <= 0:
                    raise ApiError("Enter how many units to remove")
                if amount > current + 1e-9:
                    raise ApiError(f"Only {tidy_number(current)} in stock, so {tidy_number(amount)} can't be removed")
                new_qty, kind = max(0.0, current - amount), "removed"
            elif mode == "set":
                new_qty, kind = amount, "counted"
            else:
                raise ApiError("Unknown adjustment type")
            new_qty = round(new_qty, 4)
            conn.execute("UPDATE items SET quantity = ?, updated_at = ? WHERE id = ?", (new_qty, now_iso(), item_id))
            self._log(conn, item_id, row["sku"], row["name"], new_qty - current, new_qty, kind, note)
        return self.get_item(item_id)

    def movements(self, limit=2000, item_id=None):
        limit = max(1, min(int(limit), 20000))
        if item_id is None:
            rows = self._read("SELECT * FROM movements ORDER BY id DESC LIMIT ?", (limit,))
        else:
            rows = self._read("SELECT * FROM movements WHERE item_id = ? ORDER BY id DESC LIMIT ?", (item_id, limit))
        return [movement_to_dict(r) for r in rows]

    def import_records(self, records, mode="merge", dry_run=False, source="spreadsheet"):
        """records: list of (row_number, {field: raw_value}) - only columns present in the file."""
        if mode not in ("merge", "replace"):
            raise ApiError("Unknown import mode")
        summary = {"rows": len(records), "added": 0, "updated": 0, "unchanged": 0,
                   "removed": 0, "skipped": [], "dry_run": bool(dry_run)}
        note = f"Imported from {source}"
        try:
            with self.transaction() as conn:
                if mode == "replace":
                    for row in conn.execute("SELECT * FROM items").fetchall():
                        self._log(conn, row["id"], row["sku"], row["name"], -row["quantity"], 0,
                                  "deleted", f"Removed when replacing inventory from {source}")
                        summary["removed"] += 1
                    conn.execute("DELETE FROM items")
                seen = {}
                for rownum, raw in records:
                    try:
                        sku = clean_text(raw.get("sku"), "sku")
                        if not sku:
                            raise ApiError("Missing SKU")
                        if sku.lower() in seen:
                            raise ApiError(f'Duplicate SKU "{sku}" (first seen on row {seen[sku.lower()]})')
                        existing = conn.execute("SELECT * FROM items WHERE sku = ?", (sku,)).fetchone()
                        base = {k: existing[k] for k in FIELDS} if existing else {}
                        base.update(raw)
                        item = validate_item(base)
                    except ApiError as e:
                        summary["skipped"].append({"row": rownum, "reason": str(e)})
                        continue
                    seen[sku.lower()] = rownum
                    if existing is None:
                        self._insert(conn, item, "imported", note)
                        summary["added"] += 1
                    elif any(not _same(item[k], existing[k]) for k in FIELDS):
                        self._update(conn, existing, item, "imported", note)
                        summary["updated"] += 1
                    else:
                        summary["unchanged"] += 1
                if dry_run:
                    raise _DryRun()
        except _DryRun:
            pass
        return summary

    def seed_from_file(self, path):
        """First run only: import the spreadsheet that ships in this folder."""
        with self.lock:
            done = self.conn.execute("SELECT value FROM meta WHERE key = 'seeded'").fetchone()
            count = self.conn.execute("SELECT COUNT(*) FROM items").fetchone()[0]
        if done:
            return None
        summary = None
        if count == 0 and os.path.exists(path):
            with open(path, "rb") as fh:
                sheet, records = read_spreadsheet(fh.read())
            summary = self.import_records(records, "merge", False, os.path.basename(path))
            summary["sheet"] = sheet
        with self.transaction() as conn:
            conn.execute("INSERT OR REPLACE INTO meta (key, value) VALUES ('seeded', ?)", (now_iso(),))
        return summary


# --------------------------------------------------------------------------- spreadsheet import

NS_MAIN = "http://schemas.openxmlformats.org/spreadsheetml/2006/main"
NS_REL = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"

HEADER_ALIASES = {
    "sku": ["sku", "sku #", "sku no", "item code", "item #", "item no", "item number", "product code",
            "part number", "part no", "part #", "code"],
    "name": ["item name", "name", "item", "product", "product name", "item description", "description"],
    "category": ["category", "item category", "type", "group"],
    "location": ["location", "bin", "bin location", "storage location", "aisle", "shelf"],
    "supplier": ["supplier", "vendor", "supplier name", "vendor name"],
    "quantity": ["quantity", "qty", "quantity on hand", "qty on hand", "on hand", "in stock", "stock", "units"],
    "reorder_level": ["reorder level", "reorder point", "reorder at", "min qty", "minimum qty",
                      "min stock", "minimum stock", "min"],
    "unit_cost": ["unit cost", "cost", "cost price", "purchase price"],
    "unit_price": ["unit price", "price", "sale price", "selling price", "retail price"],
    "notes": ["notes", "note", "comments", "comment", "remarks"],
}
ALIAS_LOOKUP = {alias: field for field, aliases in HEADER_ALIASES.items() for alias in aliases}
NO_HEADER_MSG = ('Couldn\'t find a header row with "SKU" and "Item Name" columns. '
                 'Make sure the sheet has those column headings.')


def _q(tag, ns=NS_MAIN):
    return f"{{{ns}}}{tag}"


def _col_index(ref):
    n = 0
    for ch in ref:
        if "A" <= ch <= "Z":
            n = n * 26 + ord(ch) - 64
        else:
            break
    return n - 1


def _rich_text(el):
    """Text of a <si>/<is> element: plain <t>, or rich-text runs <r><t>. Phonetic hints are skipped."""
    parts = []
    for child in el:
        if child.tag == _q("t"):
            parts.append(child.text or "")
        elif child.tag == _q("r"):
            t = child.find(_q("t"))
            if t is not None:
                parts.append(t.text or "")
    return "".join(parts)


def _cell_value(c, shared):
    t = c.get("t", "n")
    if t == "inlineStr":
        is_ = c.find(_q("is"))
        return _rich_text(is_) if is_ is not None else ""
    v = c.find(_q("v"))
    if v is None or v.text is None:
        return None
    text = v.text
    if t == "s":
        try:
            return shared[int(text)]
        except (ValueError, IndexError):
            return ""
    if t == "str":
        return text
    if t == "e":
        return None
    if t == "b":
        return "TRUE" if text == "1" else "FALSE"
    try:
        return float(text)
    except ValueError:
        return text


def _read_sheet(root, shared):
    rows = []
    for row in root.iter(_q("row")):
        try:
            rownum = int(row.get("r"))
        except (TypeError, ValueError):
            rownum = rows[-1][0] + 1 if rows else 1
        cells, next_col = [], 0
        for c in row.findall(_q("c")):
            ref = c.get("r")
            col = _col_index(ref) if ref else next_col
            next_col = col + 1
            if col < 0 or col > 300:
                continue
            if col >= len(cells):
                cells.extend([None] * (col + 1 - len(cells)))
            cells[col] = _cell_value(c, shared)
        rows.append((rownum, cells))
    return rows


def read_xlsx(data):
    """Returns [(sheet_name, loader)] with a sheet called 'Inventory' first."""
    try:
        zf = zipfile.ZipFile(io.BytesIO(data))
    except zipfile.BadZipFile:
        raise ApiError("This file isn't a valid .xlsx workbook")
    if sum(i.file_size for i in zf.infolist()) > MAX_UNZIPPED_BYTES:
        raise ApiError("This workbook is too large to import")
    names = set(zf.namelist())
    if "xl/workbook.xml" not in names:
        raise ApiError("This file isn't an Excel workbook (.xlsx)")
    try:
        shared = []
        if "xl/sharedStrings.xml" in names:
            shared = [_rich_text(si) for si in ET.fromstring(zf.read("xl/sharedStrings.xml")).iter(_q("si"))]
        rels = {}
        if "xl/_rels/workbook.xml.rels" in names:
            for rel in ET.fromstring(zf.read("xl/_rels/workbook.xml.rels")):
                rels[rel.get("Id")] = rel.get("Target", "")
        sheets = []
        for sh in ET.fromstring(zf.read("xl/workbook.xml")).iter(_q("sheet")):
            target = rels.get(sh.get(_q("id", NS_REL)), "")
            path = target.lstrip("/") if target.startswith("/") else posixpath.normpath(posixpath.join("xl", target))
            if path in names:
                sheets.append((sh.get("name") or "Sheet", path))
    except ET.ParseError:
        raise ApiError("This workbook couldn't be read. It may be damaged.")
    sheets.sort(key=lambda s: s[0].strip().lower() != "inventory")

    def loader(path):
        def load():
            try:
                return _read_sheet(ET.fromstring(zf.read(path)), shared)
            except ET.ParseError:
                raise ApiError("This workbook couldn't be read. It may be damaged.")
        return load

    return [(name, loader(path)) for name, path in sheets]


def read_csv(data):
    for enc in ("utf-8-sig", "cp1252"):
        try:
            text = data.decode(enc)
            break
        except UnicodeDecodeError:
            continue
    else:
        text = data.decode("latin-1")
    try:
        dialect = csv.Sniffer().sniff(text[:4096], delimiters=",;\t")
    except csv.Error:
        dialect = csv.excel
    rows = []
    for i, row in enumerate(csv.reader(io.StringIO(text), dialect)):
        # Undo the formula-safety apostrophe added by our own CSV export.
        rows.append((i + 1, [v[1:] if v[:2] in ("'=", "'+", "'@") else v for v in row]))
    return rows


def _norm_header(value):
    if value is None:
        return ""
    s = str(value).lower().replace("_", " ")
    return re.sub(r"[^a-z0-9#]+", " ", s).strip()


def _map_header(cells):
    mapping, used = {}, set()
    for idx, value in enumerate(cells):
        field = ALIAS_LOOKUP.get(_norm_header(value))
        if field and field not in used:
            mapping[idx] = field
            used.add(field)
    return mapping if {"sku", "name"} <= used else None


def records_from_rows(rows):
    for pos, (_, header_cells) in enumerate(rows[:100]):
        mapping = _map_header(header_cells)
        if not mapping:
            continue
        records = []
        for rownum, cells in rows[pos + 1:]:
            rec = {field: (cells[idx] if idx < len(cells) else None) for idx, field in mapping.items()}
            if all(v is None or (isinstance(v, str) and not v.strip()) for v in rec.values()):
                continue
            records.append((rownum, rec))
        return records
    return None


def read_spreadsheet(data):
    """Returns (sheet_name, records) from .xlsx or .csv bytes."""
    if not data:
        raise ApiError("The file is empty")
    if data[:4] == b"\xd0\xcf\x11\xe0":
        raise ApiError("Old .xls files aren't supported. Open the file in Excel and use "
                       "Save As > Excel Workbook (.xlsx), then import that.")
    if data[:2] == b"PK":
        for name, load in read_xlsx(data):
            records = records_from_rows(load())
            if records is not None:
                return name, records
        raise ApiError(NO_HEADER_MSG)
    records = records_from_rows(read_csv(data))
    if records is None:
        raise ApiError(NO_HEADER_MSG)
    return "CSV", records


# --------------------------------------------------------------------------- export

ITEM_COLUMNS = [  # header, key, width, kind, formula (xlsx only)
    ("SKU", "sku", 14, "text", None),
    ("Item Name", "name", 30, "text", None),
    ("Category", "category", 16, "text", None),
    ("Location", "location", 14, "text", None),
    ("Supplier", "supplier", 24, "text", None),
    ("Quantity", "quantity", 11, "num", None),
    ("Reorder Level", "reorder_level", 14, "num", None),
    ("Unit Cost", "unit_cost", 12, "money", None),
    ("Unit Price", "unit_price", 12, "money", None),
    ("Total Value", "total_value", 14, "money", "F{r}*H{r}"),
    ("Status", "status", 11, "text", 'IF(F{r}<=G{r},"Reorder","OK")'),
    ("Notes", "notes", 40, "text", None),
]
MOVEMENT_COLUMNS = [
    ("When", "created_at", 22, "text", None),
    ("SKU", "sku", 14, "text", None),
    ("Item Name", "item_name", 30, "text", None),
    ("Type", "kind", 12, "text", None),
    ("Change", "change", 10, "num", None),
    ("On Hand After", "quantity_after", 14, "num", None),
    ("Note", "note", 40, "text", None),
]
KIND_LABELS = {"created": "Created", "received": "Received", "removed": "Removed", "counted": "Count",
               "edited": "Edited", "imported": "Imported", "deleted": "Deleted"}
XLSX_MIME = "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"

STYLES_XML = (
    f'<?xml version="1.0" encoding="UTF-8" standalone="yes"?><styleSheet xmlns="{NS_MAIN}">'
    '<numFmts count="1"><numFmt numFmtId="164" formatCode="\\$#,##0.00"/></numFmts>'
    '<fonts count="2"><font><sz val="11"/><name val="Calibri"/></font>'
    '<font><b/><sz val="11"/><color rgb="FFFFFFFF"/><name val="Calibri"/></font></fonts>'
    '<fills count="3"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill>'
    '<fill><patternFill patternType="solid"><fgColor rgb="FF1F4E79"/><bgColor indexed="64"/></patternFill></fill></fills>'
    '<borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>'
    '<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>'
    '<cellXfs count="3"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/>'
    '<xf numFmtId="0" fontId="1" fillId="2" borderId="0" xfId="0" applyFont="1" applyFill="1"/>'
    '<xf numFmtId="164" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/></cellXfs>'
    '<cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles>'
    '</styleSheet>')


def _col_letter(i):
    s, i = "", i + 1
    while i:
        i, r = divmod(i - 1, 26)
        s = chr(65 + r) + s
    return s


def _xml_text(s):
    s = "".join(ch for ch in str(s) if ch in "\t\n\r" or ord(ch) >= 32)
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def _sheet_xml(columns, records):
    out = [f'<?xml version="1.0" encoding="UTF-8" standalone="yes"?><worksheet xmlns="{NS_MAIN}" xmlns:r="{NS_REL}">',
           '<sheetViews><sheetView workbookViewId="0"><pane ySplit="1" topLeftCell="A2" activePane="bottomLeft" '
           'state="frozen"/></sheetView></sheetViews><sheetFormatPr defaultRowHeight="15"/><cols>']
    for i, col in enumerate(columns):
        out.append(f'<col min="{i + 1}" max="{i + 1}" width="{col[2]}" customWidth="1"/>')
    out.append('</cols><sheetData><row r="1">')
    for i, col in enumerate(columns):
        out.append(f'<c r="{_col_letter(i)}1" t="inlineStr" s="1"><is><t>{_xml_text(col[0])}</t></is></c>')
    out.append("</row>")
    for r, rec in enumerate(records, start=2):
        out.append(f'<row r="{r}">')
        for i, (_, key, _, kind, formula) in enumerate(columns):
            ref, value = f"{_col_letter(i)}{r}", rec.get(key)
            f = f"<f>{_xml_text(formula.format(r=r))}</f>" if formula else ""
            style = ' s="2"' if kind == "money" else ""
            if isinstance(value, (int, float)) and not isinstance(value, bool):
                out.append(f'<c r="{ref}"{style}>{f}<v>{value}</v></c>')
            elif value not in (None, ""):
                if formula:
                    out.append(f'<c r="{ref}" t="str">{f}<v>{_xml_text(value)}</v></c>')
                else:
                    out.append(f'<c r="{ref}" t="inlineStr"><is><t xml:space="preserve">{_xml_text(value)}</t></is></c>')
        out.append("</row>")
    last = f"{_col_letter(len(columns) - 1)}{len(records) + 1}"
    out.append(f'</sheetData><autoFilter ref="A1:{last}"/></worksheet>')
    return "".join(out), last


def build_xlsx(sheets):
    """sheets: [(name, columns, records)]"""
    n = len(sheets)
    sheet_parts, defined = [], []
    for i, (name, columns, records) in enumerate(sheets):
        xml, last = _sheet_xml(columns, records)
        sheet_parts.append(xml)
        col, row = re.match(r"([A-Z]+)(\d+)", last).groups()
        defined.append(f'<definedName name="_xlnm._FilterDatabase" localSheetId="{i}" hidden="1">'
                       f"'{_xml_text(name)}'!$A$1:${col}${row}</definedName>")
    content_types = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
        '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
        '<Default Extension="xml" ContentType="application/xml"/>'
        '<Override PartName="/xl/workbook.xml" '
        'ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>'
        '<Override PartName="/xl/styles.xml" '
        'ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>'
        + "".join(f'<Override PartName="/xl/worksheets/sheet{i + 1}.xml" '
                  'ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>'
                  for i in range(n))
        + "</Types>")
    root_rels = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
        '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/'
        'officeDocument" Target="xl/workbook.xml"/></Relationships>')
    workbook = (
        f'<?xml version="1.0" encoding="UTF-8" standalone="yes"?><workbook xmlns="{NS_MAIN}" xmlns:r="{NS_REL}">'
        "<sheets>"
        + "".join(f'<sheet name="{_xml_text(name)}" sheetId="{i + 1}" r:id="rId{i + 1}"/>'
                  for i, (name, _, _) in enumerate(sheets))
        + "</sheets><definedNames>" + "".join(defined) + '</definedNames><calcPr calcId="191029" fullCalcOnLoad="1"/>'
        "</workbook>")
    wb_rels = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
        + "".join(f'<Relationship Id="rId{i + 1}" Type="{NS_REL}/worksheet" Target="worksheets/sheet{i + 1}.xml"/>'
                  for i in range(n))
        + f'<Relationship Id="rId{n + 1}" Type="{NS_REL}/styles" Target="styles.xml"/></Relationships>')
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", content_types)
        z.writestr("_rels/.rels", root_rels)
        z.writestr("xl/workbook.xml", workbook)
        z.writestr("xl/_rels/workbook.xml.rels", wb_rels)
        z.writestr("xl/styles.xml", STYLES_XML)
        for i, xml in enumerate(sheet_parts):
            z.writestr(f"xl/worksheets/sheet{i + 1}.xml", xml)
    return buf.getvalue()


def build_xlsx_export(items, movements):
    activity = [{**m, "kind": KIND_LABELS.get(m["kind"], m["kind"]),
                 "created_at": m["created_at"].replace("T", " ")[:19]} for m in movements]
    return build_xlsx([("Inventory", ITEM_COLUMNS, items), ("Activity", MOVEMENT_COLUMNS, activity)])


def build_csv(items):
    buf = io.StringIO()
    writer = csv.writer(buf, lineterminator="\r\n")
    writer.writerow([c[0] for c in ITEM_COLUMNS])
    for it in items:
        row = []
        for _, key, _, _, _ in ITEM_COLUMNS:
            v = it[key]
            # Stop spreadsheet apps from treating text like "=..." as a formula.
            if isinstance(v, str) and v[:1] in ("=", "+", "@"):
                v = "'" + v
            row.append(v)
        writer.writerow(row)
    return ("\ufeff" + buf.getvalue()).encode("utf-8")


# --------------------------------------------------------------------------- HTTP

STATIC_TYPES = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
                ".css": "text/css; charset=utf-8", ".svg": "image/svg+xml", ".png": "image/png",
                ".ico": "image/x-icon", ".json": "application/json"}


class Handler(BaseHTTPRequestHandler):
    server_version = "InventoryManager/1.0"

    def log_message(self, fmt, *args):  # keep the console quiet
        pass

    def do_GET(self):
        self._dispatch("GET")

    def do_POST(self):
        self._dispatch("POST")

    def do_PUT(self):
        self._dispatch("PUT")

    def do_DELETE(self):
        self._dispatch("DELETE")

    # ---- plumbing

    def _dispatch(self, method):
        try:
            if not self._host_allowed():
                return self._send_json({"error": "Host not allowed"}, 403)
            url = urlparse(self.path)
            path = unquote(url.path)
            if path.startswith("/api/"):
                # Custom header => browsers block cross-site pages from sending these requests.
                if method != "GET" and self.headers.get("X-Requested-With") != APP_ID:
                    return self._send_json({"error": "Missing request header"}, 403)
                return self._api(method, path, parse_qs(url.query))
            if method != "GET":
                return self._send_json({"error": "Method not allowed"}, 405)
            return self._static(path)
        except ApiError as e:
            self._send_json({"error": str(e)}, e.status)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass
        except Exception as e:  # noqa: BLE001 - report anything unexpected to the UI
            traceback.print_exc()
            self._send_json({"error": f"Unexpected error: {e}"}, 500)

    def _host_allowed(self):
        if not self.server.check_host:
            return True
        host = (self.headers.get("Host") or "").lower()
        hostname = host.split("]")[0] + "]" if host.startswith("[") else host.rsplit(":", 1)[0]
        return hostname in ("localhost", "127.0.0.1", "[::1]")

    def _body(self, limit):
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            raise ApiError("Bad Content-Length header")
        if length > limit:
            raise ApiError(f"That's too large to upload (limit {limit // (1024 * 1024) or 1} MB)", 413)
        return self.rfile.read(length) if length > 0 else b""

    def _json(self):
        raw = self._body(MAX_JSON_BYTES)
        try:
            return json.loads(raw.decode("utf-8")) if raw else {}
        except (UnicodeDecodeError, json.JSONDecodeError):
            raise ApiError("Invalid JSON in request")

    def _send(self, status, body, ctype, extra=None):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Content-Type-Options", "nosniff")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _send_json(self, obj, status=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self._send(status, body, "application/json; charset=utf-8", {"Cache-Control": "no-store"})

    def _send_download(self, body, ctype, filename):
        self._send(200, body, ctype, {"Cache-Control": "no-store",
                                      "Content-Disposition": f'attachment; filename="{filename}"'})

    def _static(self, path):
        if path in ("/", "/index.html"):
            rel = "index.html"
        elif path.startswith("/static/"):
            rel = path[len("/static/"):]
        else:
            return self._send(404, b"Not found", "text/plain; charset=utf-8")
        root = os.path.realpath(STATIC_DIR)
        full = os.path.realpath(os.path.join(root, rel))
        if not full.startswith(root + os.sep) or not os.path.isfile(full):
            return self._send(404, b"Not found", "text/plain; charset=utf-8")
        with open(full, "rb") as fh:
            body = fh.read()
        ctype = STATIC_TYPES.get(os.path.splitext(full)[1].lower(), "application/octet-stream")
        self._send(200, body, ctype, {"Cache-Control": "no-cache"})

    # ---- routes

    def _api(self, method, path, qs):
        store = self.server.store
        if path == "/api/health" and method == "GET":
            return self._send_json({"app": APP_ID, "ok": True})

        if path == "/api/items":
            if method == "GET":
                return self._send_json({"items": store.list_items(), "default_categories": DEFAULT_CATEGORIES})
            if method == "POST":
                return self._send_json({"item": store.create_item(self._json())}, 201)

        m = re.fullmatch(r"/api/items/(\d+)(/adjust|/movements)?", path)
        if m:
            item_id, sub = int(m.group(1)), m.group(2)
            if sub is None and method == "GET":
                return self._send_json({"item": store.get_item(item_id)})
            if sub is None and method == "PUT":
                return self._send_json({"item": store.update_item(item_id, self._json())})
            if sub is None and method == "DELETE":
                store.delete_item(item_id)
                return self._send_json({"ok": True})
            if sub == "/adjust" and method == "POST":
                return self._send_json({"item": store.adjust(item_id, self._json())})
            if sub == "/movements" and method == "GET":
                return self._send_json({"movements": store.movements(500, item_id)})

        if path == "/api/movements" and method == "GET":
            try:
                limit = int(qs.get("limit", ["2000"])[0])
            except ValueError:
                limit = 2000
            return self._send_json({"movements": store.movements(limit)})

        if path == "/api/import" and method == "POST":
            data = self._body(MAX_UPLOAD_BYTES)
            filename = re.sub(r"[\x00-\x1f]", "", os.path.basename(qs.get("filename", [""])[0]))[:120]
            mode = qs.get("mode", ["merge"])[0]
            dry_run = qs.get("dry_run", ["0"])[0] in ("1", "true")
            sheet, records = read_spreadsheet(data)
            summary = store.import_records(records, mode, dry_run, filename or "spreadsheet")
            summary["sheet"] = sheet
            return self._send_json(summary)

        if path in ("/api/export.xlsx", "/api/export.csv") and method == "GET":
            stamp = datetime.now().strftime("%Y-%m-%d")
            items = store.list_items()
            if path.endswith(".xlsx"):
                body = build_xlsx_export(items, store.movements(20000))
                return self._send_download(body, XLSX_MIME, f"inventory-{stamp}.xlsx")
            return self._send_download(build_csv(items), "text/csv; charset=utf-8", f"inventory-{stamp}.csv")

        raise NotFound("Not found")


class AppServer(ThreadingHTTPServer):
    daemon_threads = True
    # On Windows SO_REUSEADDR lets two servers share a port, so leave it off there.
    allow_reuse_address = os.name != "nt"

    def __init__(self, address, store, check_host):
        super().__init__(address, Handler)
        self.store = store
        self.check_host = check_host


def _already_running(port):
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        with opener.open(f"http://127.0.0.1:{port}/api/health", timeout=1.5) as r:
            return json.loads(r.read().decode("utf-8")).get("app") == APP_ID
    except Exception:  # noqa: BLE001
        return False


def _message_box(message, error=False):
    """Show a Windows message box (used when there is no console window)."""
    try:
        import ctypes
        ctypes.windll.user32.MessageBoxW(None, message, "Inventory Manager", 0x10 if error else 0x40)
    except Exception:  # noqa: BLE001
        pass


def _tray_image():
    from PIL import Image
    try:
        return Image.open(os.path.join(RESOURCE_DIR, "app.ico"))
    except OSError:
        return Image.new("RGB", (64, 64), (34, 87, 197))


def _run_in_tray(server, url, note):
    """Serve in the background; a tray icon offers Open and Quit. Returns after Quit."""
    import pystray

    def open_app(icon, item):
        webbrowser.open(url)

    def quit_app(icon, item):
        icon.stop()

    icon = pystray.Icon(
        APP_ID, _tray_image(), "Inventory Manager",
        menu=pystray.Menu(
            pystray.MenuItem("Open Inventory Manager", open_app, default=True),
            pystray.MenuItem("Quit", quit_app),
        ),
    )

    def setup(icon):
        icon.visible = True
        try:
            icon.notify(note, "Inventory Manager is running")
        except Exception:  # noqa: BLE001
            pass

    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        icon.run(setup=setup)
    finally:
        server.shutdown()


def _fail(message):
    """Show a startup error; when run as an .exe, keep the window open so it can be read."""
    if NO_CONSOLE:
        _message_box(message, error=True)
        sys.exit(1)
    print(message)
    if FROZEN:
        try:
            input("\nPress Enter to close this window.")
        except EOFError:
            pass
    sys.exit(1)


def main():
    if os.name == "nt":
        try:
            import ctypes
            ctypes.windll.kernel32.SetConsoleTitleW("Inventory Manager")
        except Exception:  # noqa: BLE001
            pass
    ap = argparse.ArgumentParser(description="Inventory Manager")
    ap.add_argument("--port", type=int, default=8765, help="port to listen on (default 8765)")
    ap.add_argument("--host", default="127.0.0.1",
                    help="address to listen on; use 0.0.0.0 to allow other computers on your network")
    ap.add_argument("--db", default=DEFAULT_DB, help="path to the database file")
    ap.add_argument("--no-browser", action="store_true", help="don't open a browser window")
    args = ap.parse_args()

    loopback = args.host in ("127.0.0.1", "localhost", "::1")
    server = None
    for port in range(args.port, args.port + 20):
        try:
            server = AppServer((args.host, port), None, check_host=loopback)
            break
        except OSError:
            if port == args.port and _already_running(port):
                url = f"http://127.0.0.1:{port}/"
                print(f"Inventory Manager is already running at {url}")
                if not args.no_browser:
                    print("Opening it in your browser...")
                    webbrowser.open(url)
                    if FROZEN and not NO_CONSOLE:
                        threading.Event().wait(3)  # let the message be seen before the window closes
                return
    if server is None:
        _fail("Couldn't find a free port to run on. Try starting it with --port 9000")

    try:
        store = Store(args.db)
    except (sqlite3.Error, OSError) as e:
        _fail(f"Couldn't open the data file {os.path.abspath(args.db)}:\n  {e}")
    server.store = store
    seed_note = ""
    try:
        summary = store.seed_from_file(SEED_FILE)
        if summary:
            seed_note = f"First run: imported {summary['added']} items from {os.path.basename(SEED_FILE)}."
    except Exception as e:  # noqa: BLE001
        seed_note = f"Note: couldn't import {os.path.basename(SEED_FILE)} on first run: {e}"
    if seed_note:
        print(seed_note)

    url = f"http://127.0.0.1:{server.server_address[1]}/"
    if NO_CONSOLE:
        if not args.no_browser:
            threading.Timer(0.5, webbrowser.open, [url]).start()
        note = "Right-click the box icon by the clock to open or quit."
        if seed_note:
            note = seed_note + "\n" + note
        try:
            _run_in_tray(server, url, note)
        except Exception as e:  # noqa: BLE001
            _fail(f"Couldn't create the tray icon, so the app was stopped:\n{e}")
        finally:
            server.server_close()
            store.close()
        return

    print("")
    print("  Inventory Manager is running")
    print(f"  Open {url} in your browser")
    if not loopback:
        print(f"  Other computers on your network can use http://<this-computer's-IP>:{server.server_address[1]}/")
    print(f"  Data file: {os.path.abspath(args.db)}")
    print("  Close this window (or press Ctrl+C) to stop.")
    print("")
    if not args.no_browser:
        threading.Timer(0.5, webbrowser.open, [url]).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        store.close()


if __name__ == "__main__":
    main()
