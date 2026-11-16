# Inventory Manager

A searchable inventory app for Windows, built from `inventory_management.xlsx`. It's written in C++ and installs like any other Windows program. It runs in its own window, works without internet access, and keeps your data on your computer.

## Install

1. Download **`InventoryManager-Setup-2.1.0.exe`** from this repository's **Releases** page (or build it yourself; see below).
2. Double-click it and follow the steps. No administrator rights are needed: it installs for your Windows account. To install it for everyone on the PC, choose that option in the first screen of the setup wizard.
3. Start **Inventory Manager** from the Start menu or the desktop shortcut.

The first time it runs, it imports the items from the bundled `inventory_management.xlsx`.

- **Windows warning:** the installer isn't digitally signed, so Windows may show *"Windows protected your PC"*. Click **More info → Run anyway**.
- **Requirements:** Windows 10 or 11, 64-bit. The app window uses the Microsoft Edge WebView2 Runtime, which comes with Windows 11 and with current Windows 10. If it's missing, the app opens in your web browser instead.
- **Uninstall:** use **Settings → Apps → Installed apps → Inventory Manager → Uninstall**. Your inventory data is kept (see below), so reinstalling or upgrading never loses it.

## What it does

- **Search**: results update as you type and match SKU, item name, category, location, supplier and notes. Several words narrow the results (`aisle 3 hub`). Press <kbd>/</kbd> to jump to the search box.
- **Filter and sort**: filter by category, location or status (Needs reorder / Out of stock / OK), and click any column heading to sort.
- **Add, edit and delete items**: click a row to edit it. *Total value* (Quantity × Unit Cost) and *Status* ("Reorder" when Quantity ≤ Reorder Level) are calculated the same way as in the spreadsheet.
- **Manufacturer and datasheet**: each item can have a manufacturer, the manufacturer's website, a datasheet link, and an attached datasheet file (PDF or similar, up to 25 MB, stored inside the app's data file).
  - The app remembers each manufacturer's website: type a known manufacturer and the website fills in automatically.
  - Small icons next to the item name open the datasheet, the attached file, or the manufacturer's website in your web browser.
  - You can type `www.example.com`; it becomes `https://www.example.com`. Anything that isn't a web address is refused.
  - **Missing datasheet:** pick it in the status filter, or click the Dashboard card, to list items without a datasheet link or file.
- **Adjust stock**: the **Adjust** button lets you *Receive* (+), *Remove* (−) or *Set count* (stocktake), with an optional note such as a PO number.
- **Dashboard**: totals, items needing reorder grouped by supplier, and stock value by category.
- **Activity**: a searchable log of every stock change.
- **Import**: load an `.xlsx` or `.csv` file. Columns are matched by heading, and you see a preview of what will change before anything is saved. *Merge* updates matching SKUs and adds new ones; *Replace* swaps out the whole inventory.
  - Link columns can be named *Manufacturer* (or *Brand*), *Manufacturer Website* (or *Website*) and *Datasheet URL* (or *Datasheet*, *Spec Sheet*), so you can fill in links for many items at once in Excel.
- **Export**: download an Excel workbook (Inventory and Activity sheets, with live formulas and clickable links) or a CSV file. Files go to your Downloads folder.

## Your data

Your inventory is stored in **`%LOCALAPPDATA%\Inventory Manager\inventory.db`**; paste that into File Explorer's address bar to open the folder. To back it up, copy that file, or use **Export → Excel workbook**. To start over from the spreadsheet, close the app, delete `inventory.db`, and open the app again.

Advanced options, added to the shortcut's *Target* after the closing quote:

```
--db "D:\Shared\inventory.db"   use a different data file
--browser                       open in your web browser instead of the app window
--host 0.0.0.0 --port 8765      also let other computers on your network open http://<this-PC>:8765/
```

With `--host 0.0.0.0` there are no logins, so anyone on your network can view and change the inventory. Use it only on a network you trust.

## Building from source

Requirements:
- **Build Tools for Visual Studio 2022 or newer**, with *Desktop development with C++*
- **Inno Setup 6** (<https://jrsoftware.org/isdl.php>), only needed for the installer
- Internet access on the first build

Then double-click **`build.bat`**. It produces:

- `build\InventoryManager.exe`, the app (one file, no runtime needed)
- `build\installer\InventoryManager-Setup-<version>.exe`, the installer

Third-party libraries are downloaded by CMake and checked against fixed SHA-256 hashes:

- [cpp-httplib](https://github.com/yhirose/cpp-httplib): local web server
- [SQLite](https://sqlite.org): database
- [nlohmann/json](https://github.com/nlohmann/json): JSON
- [pugixml](https://pugixml.org): reading Excel files
- [miniz](https://github.com/richgel999/miniz): .xlsx zip files
- [WebView2 SDK](https://www.nuget.org/packages/Microsoft.Web.WebView2): app window

### Project layout

| Path | What it is |
| --- | --- |
| `src/main.cpp` | Windows app: window, WebView2, single instance, command-line options |
| `src/server.cpp` | Local HTTP API used by the app's pages |
| `src/store.cpp` | SQLite storage, stock adjustments, import logic |
| `src/spreadsheet.cpp` | Reading and writing .xlsx and .csv files |
| `src/util.cpp` | Shared helpers: validation, numbers, text |
| `static/` | The app's pages (HTML/CSS/JS), built into the .exe |
| `installer/InventoryManager.iss` | Inno Setup installer script |
| `packaging/` | App icon (`make_icon.py` regenerates it; needs Python and Pillow) |

Version 1.0.0 was a Python version. It's still in the git history and on the v1.0.0 release.
