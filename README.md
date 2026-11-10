# Inventory Manager

A searchable inventory app built from `inventory_management.xlsx`. It runs on this computer and opens in your web browser. It doesn't need internet access, and the `.exe` doesn't need Python either.

## Get the app

The `.exe` isn't stored in the repository's files. Download **`Inventory-Manager.exe`** from this repository's **Releases** page and put it in this folder, next to `inventory_management.xlsx`. Or build it yourself with `build_exe.bat` (see *Rebuilding the exe*).

## Start it

Double-click **`Inventory Manager.exe`** (or `Inventory-Manager.exe` if you downloaded it from Releases). Your browser goes to <http://127.0.0.1:8765/>, and a blue box icon appears in the system tray by the clock. No window stays open.

- **Open it again** after closing the browser tab: double-click the tray icon, or right-click it → **Open Inventory Manager**. Double-clicking the `.exe` again does the same.
- **Stop the app:** right-click the tray icon → **Quit**.
- **Can't see the icon?** Windows may tuck it under the **^** arrow next to the clock. To keep it visible, drag it from there onto the taskbar.

- **Windows warning on first launch:** the `.exe` isn't digitally signed, so Windows may show *"Windows protected your PC"*. Click **More info → Run anyway**. You only have to do this once.
- **Desktop shortcut:** right-click `Inventory Manager.exe` → **Show more options → Send to → Desktop (create shortcut)**.
- **Keep the exe in this folder.** It stores its data in the `data` folder next to it. If you move the app to another folder or computer, move `Inventory Manager.exe` *and* the `data` folder together.

Other ways to start it, if you ever need them: `Start Inventory Manager.bat`, or `python server.py` from a terminal. Both use the same data. These run in a terminal window instead of the tray; close the window to stop.

## Rebuilding the exe

The exe is built from `server.py` and the `static` folder. After changing either one, quit the app (tray icon → **Quit**) and double-click **`build_exe.bat`**. The first build downloads the build tools (PyInstaller, Pillow and pystray) into the `build` folder, which is safe to delete; it is recreated when needed. `packaging` holds the icon and version details.

On first run, the app copies every item from `inventory_management.xlsx` into its own database. After that the spreadsheet isn't changed or read again; the app is where your inventory lives.

## What it does

- **Search**: type in the search box (press <kbd>/</kbd> to jump there). Results update as you type and match SKU, item name, category, location, supplier and notes. Several words narrow the results (`aisle 3 hub`). Matches are highlighted.
- **Filter and sort**: filter by category, location or status (Needs reorder / Out of stock / OK), and click any column heading to sort.
- **Add, edit and delete items**: click a row to edit it. *Total value* (Quantity × Unit Cost) and *Status* ("Reorder" when Quantity ≤ Reorder Level) are calculated the same way as in the spreadsheet.
- **Adjust stock**: the **Adjust** button on each row lets you *Receive* (+), *Remove* (−) or *Set count* (stocktake), with an optional note such as a PO number.
- **Dashboard**: totals (SKUs, units, stock value, retail value), the list of items needing reorder grouped by supplier, and stock value by category.
- **Activity**: a searchable log of every stock change (received, removed, counted, edited, imported, deleted).
- **Import**: load an `.xlsx` or `.csv` file. Columns are matched by heading, and you see a preview of what will change before anything is saved. *Merge* updates matching SKUs and adds new ones; *Replace* swaps out the whole inventory.
- **Export**: download an Excel workbook (Inventory and Activity sheets, with live Total Value and Status formulas) or a CSV file.

## Your data

Everything is stored in `data\inventory.db`. To back it up, copy that file, or use **Export → Excel workbook**. To start over from the spreadsheet, quit the app, delete `data\inventory.db`, and start the app again.

## Options

These work with the exe (from a terminal or in a shortcut's *Target*) and with `python server.py`:

```
"Inventory Manager.exe" --port 9000       # use a different port
"Inventory Manager.exe" --host 0.0.0.0    # let other computers on your network open it
"Inventory Manager.exe" --no-browser      # don't open a browser window automatically
```

With `--host 0.0.0.0`, Windows Firewall will ask whether to allow access; choose *Private networks*. Anyone on your network who can reach this computer can view and change the inventory, because there are no logins. Use it only on a network you trust.
