# Inventory Manager

A searchable inventory app for Windows, built from `inventory_management.xlsx`. It's written in C++ and installs like any other Windows program. It runs in its own window, works without internet access, and keeps your data on your computer.

## Install

1. Download **`InventoryManager-Setup-2.3.0.exe`** from this repository's **Releases** page (or build it yourself; see below).
2. Double-click it and follow the steps. No administrator rights are needed: it installs for your Windows account. To install it for everyone on the PC, choose that option in the first screen of the setup wizard.
3. Start **Inventory Manager** from the Start menu or the desktop shortcut.

The first time it runs, it imports the items from the bundled `inventory_management.xlsx`.

- **Windows warning:** the installer isn't digitally signed, so Windows may show *"Windows protected your PC"*. Click **More info → Run anyway**.
- **Requirements:** Windows 10 or 11, 64-bit. The app window uses the Microsoft Edge WebView2 Runtime, which comes with Windows 11 and with current Windows 10. If it's missing, the app opens in your web browser instead.
- **Uninstall:** use **Settings → Apps → Installed apps → Inventory Manager → Uninstall**. Your inventory data is kept (see below), so reinstalling or upgrading never loses it.

## What it does

- **Search**: results update as you type and match SKU, item name, category, location, supplier and notes. Several words narrow the results (`aisle 3 hub`). Press <kbd>Ctrl</kbd>+<kbd>F</kbd> or <kbd>/</kbd> to jump to the search box.
- **Filter and sort**: filter by category, location or status (Needs reorder / Out of stock / OK / Missing datasheet), and click any column heading to sort.
- **Working with items**:
  - **Click** an item to select it. **Double-click** it, or press <kbd>Enter</kbd>, to open its edit window. <kbd>↑</kbd>/<kbd>↓</kbd> move between items, and <kbd>Delete</kbd> deletes the selected one (after asking).
  - **Right-click an item** for its menu: edit, receive / remove / set count, open its datasheet or supplier website, duplicate, delete, or add a new item.
  - **Right-click anywhere else** in the Inventory view for *Add item*, *Import* and *Clear search & filters*.
  - *Total value* (Quantity × Unit Cost) and *Status* ("Reorder" when Quantity ≤ Reorder Level) are calculated the same way as in the spreadsheet.
- **Menu bar**: *File* (new item, import, export, back up, restore, settings, exit), *Edit*, *View* and *Help*. Hold <kbd>Alt</kbd> and press the underlined letter to open a menu. **Help → Keyboard and mouse shortcuts** lists all the shortcuts.
- **Supplier website and datasheet**: each item can have its supplier's website, a datasheet link, and an attached datasheet file (PDF or similar, up to 25 MB, stored inside the app's data file).
  - The app remembers each supplier's website: pick a supplier you've used before and the website fills in automatically.
  - Small icons next to the item name open the datasheet, the attached file, or the supplier's website in your web browser.
  - You can type `www.example.com`; it becomes `https://www.example.com`. Anything that isn't a web address is refused.
- **Adjust stock**: the **Adjust** button (or the right-click menu) lets you *Receive* (+), *Remove* (−) or *Set count* (stocktake), with an optional note such as a PO number.
- **Dashboard**: totals, items needing reorder grouped by supplier, items missing a datasheet, and stock value by category.
- **Activity**: a searchable log of every stock change.
- **Import**: load an `.xlsx` or `.csv` file. Columns are matched by heading, and you see a preview of what will change before anything is saved. *Merge* updates matching SKUs and adds new ones; *Replace* swaps out the whole inventory.
  - Link columns can be named *Supplier Website* (or *Website*) and *Datasheet URL* (or *Datasheet*, *Spec Sheet*), so you can fill in links for many items at once in Excel.
- **Export**: save an Excel workbook (Inventory and Activity sheets, with live formulas and clickable links) or a CSV file. Windows asks where to save it.

## Settings

**File → Settings** (<kbd>Ctrl</kbd>+<kbd>,</kbd>):

- **Currency**: used on screen and in Excel exports.
- **Appearance**: same as Windows, Light or Dark.
- **New items**: a default category and reorder level for new items.
- **Backups**: turn the daily automatic backup on or off, and choose how many to keep.
- **Data locations**: where your data and backups are, with buttons to open those folders.

## Backup and restore

- **Automatic**: once a day, when the app opens, it saves a copy of your whole inventory in the *Backups* folder. It keeps the last 10 by default.
- **Back up now**: **File → Back up inventory** (<kbd>Ctrl</kbd>+<kbd>B</kbd>) saves a backup file wherever you choose, such as a USB stick or a cloud folder.
- **Restore**: **File → Restore from backup** lists the automatic backups, or lets you pick a backup file you saved. Restoring replaces everything: items, history, attached files and settings. Your current inventory is saved to the Backups folder first, so a restore can always be undone.

## Using one inventory on several computers (Google Drive)

You can keep the inventory in a folder that syncs between your computers, such as Google Drive, OneDrive or Dropbox.

**Set it up:**

1. Install Google Drive for desktop on each computer and sign in to the same account. Create a folder in it, for example `My Drive\Inventory Manager`.
2. On the computer that has your current inventory, go to **File → Settings → Data location → Use a shared folder…**, choose that folder, and click **Use this folder**. Your inventory is copied there and Inventory Manager restarts.
3. On each other computer, do the same and choose the same folder. Pick **Use the inventory in this folder**.

**How it works:**

- **Working copy:** each computer works on its own copy and saves it to the shared folder a few seconds after each change, and when Inventory Manager closes. The status chip at the top (*Saving…*, *Saved 2 min ago*) shows where it's at, and Google Drive then syncs the file to your other computers.
- **One computer at a time:** only one computer can make changes at a time. If the inventory is open on another computer, you can open it **view only**. If that computer crashed or was turned off without closing Inventory Manager, you're offered to take over after 10 minutes, or straight away with **Open anyway**.
- **Nothing is overwritten silently:** if changes from two computers ever collide, nothing is overwritten. The changes that couldn't be saved are kept in the *Backups* folder (as `unsaved-changes-….db`, restorable from **File → Restore from backup**), and a message explains what happened.
- **Switching computers:** close Inventory Manager on one computer and give Google Drive a moment to finish syncing before opening it on another.
- **Going back:** **Settings → Data location → Keep data on this computer only…** switches back. It can keep the shared inventory, or go back to this computer's earlier data.

Backups and settings like the window position stay on each computer.

## Your data

Your inventory is stored in **`%LOCALAPPDATA%\Inventory Manager\inventory.db`** (or in the shared folder you chose, with a working copy in `%LOCALAPPDATA%\Inventory Manager\shared`), and backups in the *Backups* folder in `%LOCALAPPDATA%\Inventory Manager`. **Help → Open data folder** opens that folder. To start over from the spreadsheet, close the app, delete `inventory.db`, and open the app again.

Upgrading from 2.1.0: the old *Manufacturer* and *Manufacturer website* values aren't shown any more, but they're kept in the data file, not deleted.

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
| `src/shared.cpp` | Shared-folder mode: lock note, saving to and loading from the shared folder, conflict detection |
| `src/store.cpp` | SQLite storage, stock adjustments, import logic |
| `src/spreadsheet.cpp` | Reading and writing .xlsx and .csv files |
| `src/util.cpp` | Shared helpers: validation, numbers, text |
| `static/` | The app's pages (HTML/CSS/JS), built into the .exe |
| `installer/InventoryManager.iss` | Inno Setup installer script |
| `packaging/` | App icon (`make_icon.py` regenerates it; needs Python and Pillow) |

Version 1.0.0 was a Python version. It's still in the git history and on the v1.0.0 release.
