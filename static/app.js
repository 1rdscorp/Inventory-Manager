'use strict';

(() => {
  // ------------------------------------------------------------------ helpers
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];

  const moneyFmt = new Intl.NumberFormat('en-US', { style: 'currency', currency: 'USD' });
  const numFmt = new Intl.NumberFormat('en-US', { maximumFractionDigits: 2 });
  const fmtMoney = (v) => moneyFmt.format(v || 0);
  const fmtNum = (v) => numFmt.format(v || 0);
  const fmtSigned = (v) => (v > 0 ? '+' : v < 0 ? '−' : '±') + numFmt.format(Math.abs(v));
  const fmtWhen = (iso) => {
    const d = new Date(iso);
    return isNaN(d) ? iso : d.toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' });
  };
  const plural = (n, word, many = word + 's') => `${fmtNum(n)} ${n === 1 ? word : many}`;
  const collator = new Intl.Collator(undefined, { numeric: true, sensitivity: 'base' });

  const ESC = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
  const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ESC[c]);

  /** Escape text and wrap every occurrence of any search term in <mark>. */
  function hl(text, terms) {
    text = String(text ?? '');
    if (!terms.length || !text) return esc(text);
    const lower = text.toLowerCase();
    const ranges = [];
    for (const t of terms) {
      let i = lower.indexOf(t);
      while (i !== -1) {
        ranges.push([i, i + t.length]);
        i = lower.indexOf(t, i + t.length);
      }
    }
    if (!ranges.length) return esc(text);
    ranges.sort((a, b) => a[0] - b[0]);
    const merged = [];
    for (const r of ranges) {
      const last = merged[merged.length - 1];
      if (last && r[0] <= last[1]) last[1] = Math.max(last[1], r[1]);
      else merged.push([...r]);
    }
    let out = '';
    let pos = 0;
    for (const [a, b] of merged) {
      out += esc(text.slice(pos, a)) + '<mark>' + esc(text.slice(a, b)) + '</mark>';
      pos = b;
    }
    return out + esc(text.slice(pos));
  }

  const termsOf = (q) => q.toLowerCase().split(/\s+/).filter(Boolean);

  function cmp(a, b) {
    if (typeof a === 'number' && typeof b === 'number') return a - b;
    return collator.compare(String(a ?? ''), String(b ?? ''));
  }

  async function api(method, url, body) {
    const headers = { 'X-Requested-With': 'inventory-manager' };
    let payload;
    if (body instanceof Blob) {
      payload = body;
      headers['Content-Type'] = 'application/octet-stream';
    } else if (body !== undefined) {
      payload = JSON.stringify(body);
      headers['Content-Type'] = 'application/json';
    }
    let res;
    try {
      res = await fetch(url, { method, headers, body: payload });
    } catch {
      throw new Error('Can’t reach Inventory Manager. Make sure its window is still open, then try again.');
    }
    let data = null;
    try { data = await res.json(); } catch { /* not JSON */ }
    if (!res.ok) throw new Error((data && data.error) || `Request failed (${res.status})`);
    return data;
  }

  function toast(msg, kind = 'ok') {
    const el = document.createElement('div');
    el.className = `toast toast-${kind}`;
    el.textContent = msg;
    $('#toasts').append(el);
    setTimeout(() => {
      el.classList.add('out');
      setTimeout(() => el.remove(), 250);
    }, kind === 'error' ? 6000 : 3200);
  }

  function showError(el, msg) {
    el.textContent = msg || '';
    el.hidden = !msg;
  }

  function confirmBox({ title, message, ok = 'OK', danger = false }) {
    const dlg = $('#confirmDialog');
    $('#confirmTitle').textContent = title;
    $('#confirmMsg').textContent = message;
    const okBtn = $('#confirmOk');
    okBtn.textContent = ok;
    okBtn.className = danger ? 'btn btn-danger-solid' : 'btn btn-primary';
    dlg.returnValue = '';
    dlg.showModal();
    okBtn.focus();
    return new Promise((resolve) => {
      const form = dlg.querySelector('form');
      let done = false;
      const finish = (ok) => {
        if (done) return;
        done = true;
        form.removeEventListener('submit', onSubmit);
        dlg.removeEventListener('close', onClose);
        resolve(ok);
      };
      // The button's submit is immediate; 'close' covers the Esc key.
      const onSubmit = (e) => finish(e.submitter?.value === 'ok');
      const onClose = () => finish(dlg.returnValue === 'ok');
      form.addEventListener('submit', onSubmit);
      dlg.addEventListener('close', onClose);
    });
  }

  const KIND_LABELS = {
    created: 'Created', received: 'Received', removed: 'Removed', counted: 'Count',
    edited: 'Edited', imported: 'Imported', deleted: 'Deleted',
  };
  const kindBadge = (k) => `<span class="kind kind-${esc(k)}">${esc(KIND_LABELS[k] || k)}</span>`;

  function statusBadge(it) {
    if (it.quantity <= 0) return '<span class="badge badge-out">Out of stock</span>';
    if (it.status === 'Reorder') return '<span class="badge badge-reorder">Reorder</span>';
    return '<span class="badge badge-ok">OK</span>';
  }

  // ------------------------------------------------------------------ state
  const state = {
    items: [],
    defaults: [],
    movements: [],
    movementsStale: true,
    view: 'inventory',
    q: '', category: '', location: '', status: '',
    sort: { key: 'sku', dir: 1 },
    actQ: '', actKind: '',
    editing: null,     // item being edited in the item dialog (null = new)
    adjusting: null,   // item in the adjust dialog
    importBlob: null,
    importName: '',
  };

  async function loadItems() {
    const data = await api('GET', '/api/items');
    state.defaults = data.default_categories || [];
    state.items = data.items.map((it) => ({
      ...it,
      _hay: [it.sku, it.name, it.category, it.location, it.supplier, it.notes].join('\u0001').toLowerCase(),
    }));
    state.movementsStale = true;
  }

  async function loadMovements() {
    const data = await api('GET', '/api/movements?limit=5000');
    state.movements = data.movements.map((m) => ({
      ...m, _hay: [m.sku, m.item_name, m.note, KIND_LABELS[m.kind]].join('\u0001').toLowerCase(),
    }));
    state.movementsStale = false;
  }

  const findItem = (id) => state.items.find((it) => it.id === id);

  function distinct(key) {
    return [...new Set(state.items.map((it) => it[key]).filter(Boolean))].sort(collator.compare);
  }

  // ------------------------------------------------------------------ rendering: shared
  function fillSelect(sel, values, allLabel) {
    const current = sel.value;
    sel.innerHTML = `<option value="">${esc(allLabel)}</option>` +
      values.map((v) => `<option value="${esc(v)}">${esc(v)}</option>`).join('');
    sel.value = values.includes(current) ? current : '';
  }

  function fillDatalist(dl, values) {
    dl.innerHTML = values.map((v) => `<option value="${esc(v)}"></option>`).join('');
  }

  function renderMeta() {
    const cats = distinct('category');
    fillSelect($('#fCategory'), cats, 'All categories');
    fillSelect($('#fLocation'), distinct('location'), 'All locations');
    state.category = $('#fCategory').value;
    state.location = $('#fLocation').value;
    const allCats = [...new Set([...state.defaults, ...cats])].sort(collator.compare);
    fillDatalist($('#dlCategories'), allCats);
    fillDatalist($('#dlLocations'), distinct('location'));
    fillDatalist($('#dlSuppliers'), distinct('supplier'));
    const n = state.items.filter((it) => it.status === 'Reorder').length;
    const badge = $('#reorderBadge');
    badge.textContent = n;
    badge.hidden = n === 0;
    badge.title = `${plural(n, 'item')} need reordering`;
  }

  function renderAll() {
    renderMeta();
    renderInventory();
    if (state.view === 'dashboard') renderDashboard();
    if (state.view === 'activity') refreshActivity();
  }

  // ------------------------------------------------------------------ inventory view
  function filteredItems() {
    const terms = termsOf(state.q);
    const list = state.items.filter((it) => {
      if (state.category && it.category !== state.category) return false;
      if (state.location && it.location !== state.location) return false;
      if (state.status === 'Reorder' && it.status !== 'Reorder') return false;
      if (state.status === 'OK' && it.status !== 'OK') return false;
      if (state.status === 'Out' && it.quantity > 0) return false;
      return terms.every((t) => it._hay.includes(t));
    });
    const { key, dir } = state.sort;
    list.sort((a, b) => cmp(a[key], b[key]) * dir || cmp(a.sku, b.sku));
    return list;
  }

  const isFiltered = () => Boolean(state.q.trim() || state.category || state.location || state.status);

  function renderInventory() {
    const list = filteredItems();
    const terms = termsOf(state.q);
    const tbody = $('#itemsTable tbody');

    tbody.innerHTML = list.map((it) => `
      <tr data-id="${it.id}" tabindex="0" class="${it.status === 'Reorder' ? 'is-low' : ''}">
        <td class="mono nowrap">${hl(it.sku, terms)}</td>
        <td class="name">${hl(it.name, terms)}${it.notes ? `<div class="sub">${hl(it.notes.length > 90 ? it.notes.slice(0, 90) + '…' : it.notes, terms)}</div>` : ''}</td>
        <td class="nowrap">${hl(it.category, terms)}</td>
        <td class="nowrap hide-sm">${hl(it.location, terms)}</td>
        <td class="hide-sm">${hl(it.supplier, terms)}</td>
        <td class="num qty">${fmtNum(it.quantity)}</td>
        <td class="num muted hide-sm">${fmtNum(it.reorder_level)}</td>
        <td class="num hide-sm">${fmtMoney(it.unit_cost)}</td>
        <td class="num hide-sm">${fmtMoney(it.unit_price)}</td>
        <td class="num strong">${fmtMoney(it.total_value)}</td>
        <td class="nowrap">${statusBadge(it)}</td>
        <td class="col-actions"><button type="button" class="btn btn-sm" data-action="adjust" title="Receive, remove or count stock">Adjust</button></td>
      </tr>`).join('');

    // Sort indicators
    $$('#itemsTable th[data-sort]').forEach((th) => {
      if (th.dataset.sort === state.sort.key) th.setAttribute('aria-sort', state.sort.dir > 0 ? 'ascending' : 'descending');
      else th.removeAttribute('aria-sort');
    });

    // Summary line for the current results
    const units = list.reduce((s, it) => s + it.quantity, 0);
    const value = list.reduce((s, it) => s + it.total_value, 0);
    const total = state.items.length;
    const filtered = isFiltered();
    $('#summary').innerHTML = total === 0 ? '' :
      `<span>${filtered ? `Showing <b>${fmtNum(list.length)}</b> of ${plural(total, 'item')}` : `<b>${plural(total, 'item')}</b>`}</span>` +
      `<span class="dot">·</span><span>${plural(units, 'unit')}</span>` +
      `<span class="dot">·</span><span>${fmtMoney(value)} stock value</span>` +
      (filtered ? '<button type="button" class="link-btn" data-action="clear">Clear search &amp; filters</button>' : '');

    const empty = $('#emptyState');
    $('#itemsTable').hidden = list.length === 0;
    if (total === 0) {
      empty.innerHTML = `<div class="empty-title">No items yet</div>
        <p>Add your first item, or import an Excel or CSV file.</p>
        <div class="empty-actions"><button type="button" class="btn btn-primary" data-action="add">Add item</button>
        <button type="button" class="btn" data-action="import">Import a spreadsheet</button></div>`;
      empty.hidden = false;
    } else if (list.length === 0) {
      empty.innerHTML = `<div class="empty-title">No matching items</div>
        <p>${state.q.trim() ? `Nothing matches “${esc(state.q.trim())}”` : 'Nothing matches these filters'}.</p>
        <div class="empty-actions"><button type="button" class="btn" data-action="clear">Clear search &amp; filters</button></div>`;
      empty.hidden = false;
    } else {
      empty.hidden = true;
    }
  }

  function clearFilters() {
    state.q = state.category = state.location = state.status = '';
    $('#search').value = '';
    $('#fCategory').value = $('#fLocation').value = $('#fStatus').value = '';
    renderInventory();
  }

  // ------------------------------------------------------------------ dashboard view
  function renderDashboard() {
    const items = state.items;
    const units = items.reduce((s, it) => s + it.quantity, 0);
    const value = items.reduce((s, it) => s + it.total_value, 0);
    const retail = items.reduce((s, it) => s + it.quantity * it.unit_price, 0);
    const reorder = items.filter((it) => it.status === 'Reorder');
    const out = items.filter((it) => it.quantity <= 0).length;
    const cats = new Set(items.map((it) => it.category).filter(Boolean)).size;
    const avgCost = items.length ? items.reduce((s, it) => s + it.unit_cost, 0) / items.length : 0;

    const card = (label, val, sub = '', extra = '') =>
      `<div class="kpi ${extra}"><div class="kpi-label">${label}</div><div class="kpi-value">${val}</div>${sub ? `<div class="kpi-sub">${sub}</div>` : ''}</div>`;
    $('#kpis').innerHTML = [
      card('Total SKUs', fmtNum(items.length), `${plural(cats, 'category', 'categories')}`),
      card('Units in stock', fmtNum(units)),
      card('Stock value', fmtMoney(value), 'quantity × unit cost'),
      card('Retail value', fmtMoney(retail), 'quantity × unit price'),
      card('Avg unit cost', fmtMoney(avgCost)),
      `<button type="button" class="kpi kpi-action ${reorder.length ? 'kpi-warn' : ''}" data-action="show-reorder">
        <div class="kpi-label">Needs reorder</div><div class="kpi-value">${fmtNum(reorder.length)}</div>
        <div class="kpi-sub">${out ? `${fmtNum(out)} out of stock · ` : ''}View list →</div></button>`,
    ].join('');

    // Reorder list, grouped by supplier so it reads like a shopping list
    reorder.sort((a, b) => cmp(a.supplier, b.supplier) || cmp(a.sku, b.sku));
    $('#reorderCount').textContent = reorder.length ? plural(reorder.length, 'item') : '';
    $('#reorderTable tbody').innerHTML = reorder.map((it) => `
      <tr data-id="${it.id}" tabindex="0">
        <td class="mono nowrap">${esc(it.sku)}</td>
        <td class="name">${esc(it.name)}<div class="sub show-sm">${esc(it.supplier)}</div></td>
        <td class="hide-sm">${esc(it.supplier)}</td>
        <td class="num qty ${it.quantity <= 0 ? 'zero' : ''}">${fmtNum(it.quantity)}</td>
        <td class="num muted">${fmtNum(it.reorder_level)}</td>
        <td class="col-actions"><button type="button" class="btn btn-sm" data-action="receive">Receive</button></td>
      </tr>`).join('');
    $('#reorderTable').hidden = reorder.length === 0;
    $('#reorderEmpty').hidden = reorder.length !== 0 || items.length === 0;

    // Value by category
    const groups = new Map();
    for (const it of items) {
      const k = it.category || 'Uncategorized';
      const g = groups.get(k) || { name: k, value: 0, count: 0, units: 0, raw: it.category };
      g.value += it.total_value; g.count += 1; g.units += it.quantity;
      groups.set(k, g);
    }
    const rows = [...groups.values()].sort((a, b) => b.value - a.value);
    const max = Math.max(1, ...rows.map((r) => r.value));
    const bars = $('#catBars');
    bars.innerHTML = rows.length ? rows.map((r) => `
      <button type="button" class="bar-row" data-category="${esc(r.raw)}" title="Show ${esc(r.name)} items">
        <span class="bar-label"><span class="bar-name">${esc(r.name)}</span><span class="bar-meta">${plural(r.count, 'item')} · ${plural(r.units, 'unit')}</span></span>
        <span class="bar-track"><span class="bar-fill"></span></span>
        <span class="bar-value">${fmtMoney(r.value)}</span>
      </button>`).join('') : '<div class="empty small">No items yet.</div>';
    $$('.bar-fill', bars).forEach((el, i) => { el.style.width = `${Math.max(1.5, (rows[i].value / max) * 100)}%`; });
  }

  // ------------------------------------------------------------------ activity view
  async function refreshActivity() {
    if (state.movementsStale) {
      try { await loadMovements(); } catch (e) { toast(e.message, 'error'); }
    }
    renderActivity();
  }

  function renderActivity() {
    const terms = termsOf(state.actQ);
    const list = state.movements.filter((m) =>
      (!state.actKind || m.kind === state.actKind) && terms.every((t) => m._hay.includes(t)));
    $('#actTable tbody').innerHTML = list.slice(0, 1000).map((m) => `
      <tr ${m.item_id && findItem(m.item_id) ? `data-id="${m.item_id}" tabindex="0"` : 'class="no-click"'}>
        <td class="nowrap muted">${esc(fmtWhen(m.created_at))}</td>
        <td class="mono nowrap">${hl(m.sku, terms)}</td>
        <td class="name">${hl(m.item_name, terms)}${m.note ? `<div class="sub show-sm">${hl(m.note, terms)}</div>` : ''}</td>
        <td>${kindBadge(m.kind)}</td>
        <td class="num ${m.change > 0 ? 'pos' : m.change < 0 ? 'neg' : 'muted'}">${fmtSigned(m.change)}</td>
        <td class="num">${fmtNum(m.quantity_after)}</td>
        <td class="hide-sm muted">${hl(m.note, terms)}</td>
      </tr>`).join('');
    const filtered = state.actQ.trim() || state.actKind;
    $('#actSummary').innerHTML = state.movements.length
      ? (filtered ? `Showing <b>${fmtNum(list.length)}</b> of ${plural(state.movements.length, 'entry', 'entries')}` : `<b>${plural(state.movements.length, 'entry', 'entries')}</b>, newest first`) +
        (list.length > 1000 ? ' (first 1,000 shown — search to narrow down)' : '')
      : '';
    $('#actTable').hidden = list.length === 0;
    const empty = $('#actEmpty');
    empty.hidden = list.length !== 0;
    empty.innerHTML = state.movements.length
      ? '<div class="empty-title">No matching activity</div>'
      : '<div class="empty-title">No activity yet</div><p>Stock changes will be listed here.</p>';
  }

  // ------------------------------------------------------------------ views / navigation
  function setView(view, { focus = false } = {}) {
    if (!['inventory', 'dashboard', 'activity'].includes(view)) view = 'inventory';
    state.view = view;
    $$('.tab').forEach((t) => {
      if (t.dataset.view === view) t.setAttribute('aria-current', 'page');
      else t.removeAttribute('aria-current');
    });
    $$('.view').forEach((s) => { s.hidden = s.id !== `view-${view}`; });
    if (view === 'dashboard') renderDashboard();
    if (view === 'activity') refreshActivity();
    if (location.hash.slice(1) !== view) history.replaceState(null, '', `#${view}`);
    if (focus && view === 'inventory') $('#search').focus();
  }

  // ------------------------------------------------------------------ item dialog
  const itemForm = $('#itemForm');
  const NUM_FIELDS = ['quantity', 'reorder_level', 'unit_cost', 'unit_price'];

  function updateItemCalc() {
    const v = (name) => parseFloat(itemForm.elements[name].value) || 0;
    const qty = v('quantity'); const level = v('reorder_level'); const cost = v('unit_cost'); const price = v('unit_price');
    const margin = price > 0 ? ((price - cost) / price) * 100 : null;
    const status = qty <= 0 ? '<span class="badge badge-out">Out of stock</span>'
      : qty <= level ? '<span class="badge badge-reorder">Reorder</span>' : '<span class="badge badge-ok">OK</span>';
    $('#itemCalc').innerHTML =
      `<span>Total value <b>${fmtMoney(qty * cost)}</b></span>` +
      (margin !== null ? `<span>Margin <b>${numFmt.format(margin)}%</b></span>` : '') +
      `<span>Status ${status}</span>`;
  }

  async function openItemDialog(item = null) {
    state.editing = item;
    itemForm.reset();
    showError($('#itemError'), '');
    $('#itemDialogTitle').textContent = item ? `Edit ${item.sku}` : 'Add item';
    $('#saveItemBtn').textContent = item ? 'Save changes' : 'Add item';
    $('#deleteItemBtn').hidden = !item;
    $('#adjustFromItemBtn').hidden = !item;
    if (item) {
      for (const f of ['sku', 'name', 'category', 'location', 'supplier', 'notes', ...NUM_FIELDS]) {
        itemForm.elements[f].value = item[f] ?? '';
      }
    } else if (state.category) {
      itemForm.elements.category.value = state.category;
    }
    updateItemCalc();
    const hist = $('#itemHistory');
    hist.hidden = true;
    $('#itemDialog').showModal();
    itemForm.elements[item ? 'name' : 'sku'].focus();

    if (item) {
      try {
        const { movements } = await api('GET', `/api/items/${item.id}/movements`);
        if (state.editing !== item) return;
        $('#itemHistoryList').innerHTML = movements.slice(0, 25).map((m) => `
          <li><span class="h-when">${esc(fmtWhen(m.created_at))}</span>${kindBadge(m.kind)}
          <span class="h-change ${m.change > 0 ? 'pos' : m.change < 0 ? 'neg' : 'muted'}">${fmtSigned(m.change)}</span>
          <span class="h-after">→ ${fmtNum(m.quantity_after)}</span>
          <span class="h-note">${esc(m.note)}</span></li>`).join('');
        hist.hidden = movements.length === 0;
      } catch { /* history is optional */ }
    }
  }

  itemForm.addEventListener('input', (e) => {
    if (NUM_FIELDS.includes(e.target.name)) updateItemCalc();
  });

  itemForm.addEventListener('submit', async (e) => {
    e.preventDefault();
    const err = $('#itemError');
    const data = Object.fromEntries(new FormData(itemForm));
    if (!data.sku.trim()) { showError(err, 'SKU is required.'); itemForm.elements.sku.focus(); return; }
    if (!data.name.trim()) { showError(err, 'Item name is required.'); itemForm.elements.name.focus(); return; }
    for (const f of NUM_FIELDS) {
      const el = itemForm.elements[f];
      if (el.validity.badInput || (el.value !== '' && Number(el.value) < 0)) {
        showError(err, `${el.closest('label').querySelector('span').textContent} must be a number of 0 or more.`);
        el.focus();
        return;
      }
    }
    const btn = $('#saveItemBtn');
    btn.disabled = true;
    try {
      const editing = state.editing;
      const res = editing
        ? await api('PUT', `/api/items/${editing.id}`, data)
        : await api('POST', '/api/items', data);
      $('#itemDialog').close();
      await loadItems();
      renderAll();
      toast(editing ? `Saved ${res.item.sku}` : `Added ${res.item.sku} · ${res.item.name}`);
    } catch (ex) {
      showError(err, ex.message);
    } finally {
      btn.disabled = false;
    }
  });

  $('#deleteItemBtn').addEventListener('click', async () => {
    const item = state.editing;
    if (!item) return;
    const ok = await confirmBox({
      title: `Delete ${item.sku}?`,
      message: `“${item.name}” will be removed from your inventory. Its past stock changes stay in the Activity log.`,
      ok: 'Delete item', danger: true,
    });
    if (!ok) return;
    try {
      await api('DELETE', `/api/items/${item.id}`);
      $('#itemDialog').close();
      await loadItems();
      renderAll();
      toast(`Deleted ${item.sku}`);
    } catch (ex) {
      showError($('#itemError'), ex.message);
    }
  });

  $('#adjustFromItemBtn').addEventListener('click', () => {
    const item = state.editing;
    $('#itemDialog').close();
    if (item) openAdjust(findItem(item.id) || item);
  });

  // ------------------------------------------------------------------ adjust dialog
  const adjustForm = $('#adjustForm');
  const ADJ_LABELS = { add: 'Quantity received', remove: 'Quantity to remove', set: 'Counted quantity on hand' };

  function updateAdjustPreview() {
    const item = state.adjusting;
    if (!item) return;
    const mode = adjustForm.elements.mode.value;
    $('#adjAmountLabel').textContent = ADJ_LABELS[mode];
    const raw = adjustForm.elements.amount.value;
    const amt = parseFloat(raw);
    const preview = $('#adjPreview');
    if (raw === '' || isNaN(amt)) {
      preview.innerHTML = `On hand now: <b>${fmtNum(item.quantity)}</b>`;
      preview.className = 'adj-preview';
      return;
    }
    let next = mode === 'add' ? item.quantity + amt : mode === 'remove' ? item.quantity - amt : amt;
    next = Math.round(next * 10000) / 10000;
    const bad = next < 0 || amt < 0;
    const low = !bad && next <= item.reorder_level;
    preview.className = `adj-preview ${bad ? 'is-bad' : low ? 'is-low' : ''}`;
    preview.innerHTML = bad
      ? `Only ${fmtNum(item.quantity)} in stock`
      : `${fmtNum(item.quantity)} → <b>${fmtNum(next)}</b> on hand${low ? ' · at or below reorder level' : ''}`;
  }

  function openAdjust(item, mode = 'add') {
    state.adjusting = item;
    adjustForm.reset();
    adjustForm.elements.mode.value = mode;
    showError($('#adjError'), '');
    $('#adjName').textContent = item.name;
    $('#adjSub').innerHTML = `<span class="mono">${esc(item.sku)}</span>${item.location ? ` · ${esc(item.location)}` : ''} · Reorder at ${fmtNum(item.reorder_level)}`;
    updateAdjustPreview();
    $('#adjustDialog').showModal();
    adjustForm.elements.amount.focus();
  }

  adjustForm.addEventListener('input', updateAdjustPreview);
  adjustForm.addEventListener('change', (e) => {
    if (e.target.name === 'mode') { updateAdjustPreview(); adjustForm.elements.amount.focus(); }
  });

  adjustForm.addEventListener('submit', async (e) => {
    e.preventDefault();
    const item = state.adjusting;
    const data = Object.fromEntries(new FormData(adjustForm));
    const err = $('#adjError');
    if (data.amount === '' || isNaN(parseFloat(data.amount))) {
      showError(err, 'Enter a quantity.');
      adjustForm.elements.amount.focus();
      return;
    }
    try {
      const { item: updated } = await api('POST', `/api/items/${item.id}/adjust`, data);
      $('#adjustDialog').close();
      await loadItems();
      renderAll();
      const amt = fmtNum(parseFloat(data.amount));
      const verb = data.mode === 'add' ? `Received ${amt} ×` : data.mode === 'remove' ? `Removed ${amt} ×` : 'Counted';
      toast(`${verb} ${updated.name} — ${fmtNum(updated.quantity)} on hand`);
    } catch (ex) {
      showError(err, ex.message);
    }
  });

  // ------------------------------------------------------------------ import dialog
  const importForm = $('#importForm');

  function openImport() {
    importForm.reset();
    state.importBlob = null;
    state.importName = '';
    $('#importFileLabel').textContent = 'Choose a file…';
    $('#importPreview').hidden = true;
    showError($('#importError'), '');
    $('#importSubmit').disabled = true;
    $('#importSubmit').textContent = 'Import';
    $('#importDialog').showModal();
  }

  const importMode = () => importForm.elements.importMode.value;

  async function previewImport() {
    const preview = $('#importPreview');
    const submit = $('#importSubmit');
    showError($('#importError'), '');
    submit.disabled = true;
    if (!state.importBlob) { preview.hidden = true; return; }
    preview.hidden = false;
    preview.innerHTML = '<span class="muted">Reading file…</span>';
    try {
      const s = await api('POST', importUrl(true), state.importBlob);
      const changes = s.added + s.updated + s.removed;
      const parts = [];
      if (s.removed) parts.push(`<li class="neg"><b>${fmtNum(s.removed)}</b> current ${s.removed === 1 ? 'item' : 'items'} removed</li>`);
      parts.push(`<li><b>${fmtNum(s.added)}</b> new ${s.added === 1 ? 'item' : 'items'} added</li>`);
      if (importMode() === 'merge') {
        parts.push(`<li><b>${fmtNum(s.updated)}</b> existing ${s.updated === 1 ? 'item' : 'items'} updated</li>`);
        parts.push(`<li class="muted"><b>${fmtNum(s.unchanged)}</b> already up to date</li>`);
      }
      if (s.skipped.length) parts.push(`<li class="warn"><b>${fmtNum(s.skipped.length)}</b> ${s.skipped.length === 1 ? 'row' : 'rows'} skipped</li>`);
      const skipped = s.skipped.slice(0, 8).map((k) => `<li>Row ${k.row}: ${esc(k.reason)}</li>`).join('') +
        (s.skipped.length > 8 ? `<li>…and ${s.skipped.length - 8} more</li>` : '');
      preview.innerHTML =
        `<div class="ip-head">Found ${plural(s.rows, 'row')} ${s.sheet === 'CSV' ? 'in the file' : `on sheet “${esc(s.sheet)}”`}. Importing will:</div>` +
        `<ul class="ip-stats">${parts.join('')}</ul>` +
        (skipped ? `<ul class="ip-skipped">${skipped}</ul>` : '');
      submit.disabled = changes === 0;
      submit.textContent = changes === 0 ? 'Nothing to import' : 'Import';
    } catch (ex) {
      preview.hidden = true;
      showError($('#importError'), ex.message);
    }
  }

  const importUrl = (dry) =>
    `/api/import?mode=${encodeURIComponent(importMode())}&dry_run=${dry ? 1 : 0}&filename=${encodeURIComponent(state.importName)}`;

  $('#importFile').addEventListener('change', (e) => {
    const file = e.target.files[0];
    if (!file) return;
    if (file.size > 25 * 1024 * 1024) {
      showError($('#importError'), 'That file is larger than 25 MB.');
      return;
    }
    state.importBlob = file;
    state.importName = file.name;
    $('#importFileLabel').textContent = file.name;
    previewImport();
  });
  importForm.addEventListener('change', (e) => {
    if (e.target.name === 'importMode' && state.importBlob) previewImport();
  });

  importForm.addEventListener('submit', async (e) => {
    e.preventDefault();
    if (!state.importBlob) return;
    if (importMode() === 'replace') {
      const ok = await confirmBox({
        title: 'Replace all items?',
        message: `Every item currently in Inventory Manager will be removed and replaced with the contents of “${state.importName}”. Consider exporting a backup first.`,
        ok: 'Replace inventory', danger: true,
      });
      if (!ok) return;
    }
    const submit = $('#importSubmit');
    submit.disabled = true;
    try {
      const s = await api('POST', importUrl(false), state.importBlob);
      $('#importDialog').close();
      await loadItems();
      renderAll();
      const bits = [`${fmtNum(s.added)} added`];
      if (s.updated) bits.push(`${fmtNum(s.updated)} updated`);
      if (s.removed) bits.push(`${fmtNum(s.removed)} removed`);
      if (s.skipped.length) bits.push(`${fmtNum(s.skipped.length)} skipped`);
      toast(`Import complete: ${bits.join(', ')}`);
    } catch (ex) {
      showError($('#importError'), ex.message);
      submit.disabled = false;
    }
  });

  // ------------------------------------------------------------------ events
  $$('.tab').forEach((t) => t.addEventListener('click', () => setView(t.dataset.view)));
  window.addEventListener('hashchange', () => setView(location.hash.slice(1)));

  $('#addBtn').addEventListener('click', () => openItemDialog());
  $('#importBtn').addEventListener('click', openImport);

  $('#search').addEventListener('input', (e) => { state.q = e.target.value; renderInventory(); });
  $('#search').addEventListener('keydown', (e) => {
    if (e.key === 'Escape' && e.target.value) { e.preventDefault(); e.target.value = ''; state.q = ''; renderInventory(); }
  });
  $('#fCategory').addEventListener('change', (e) => { state.category = e.target.value; renderInventory(); });
  $('#fLocation').addEventListener('change', (e) => { state.location = e.target.value; renderInventory(); });
  $('#fStatus').addEventListener('change', (e) => { state.status = e.target.value; renderInventory(); });

  $('#itemsTable thead').addEventListener('click', (e) => {
    const th = e.target.closest('th[data-sort]');
    if (!th) return;
    const key = th.dataset.sort;
    const numeric = ['quantity', 'reorder_level', 'unit_cost', 'unit_price', 'total_value'].includes(key);
    state.sort = state.sort.key === key ? { key, dir: -state.sort.dir } : { key, dir: numeric ? -1 : 1 };
    renderInventory();
  });

  // Row clicks (inventory, reorder list and activity rows open the item)
  function onRowActivate(e) {
    const btn = e.target.closest('button[data-action]');
    const tr = e.target.closest('tr[data-id]');
    if (btn && tr) {
      const item = findItem(Number(tr.dataset.id));
      if (item) openAdjust(item, 'add');
      return;
    }
    if (tr) {
      const item = findItem(Number(tr.dataset.id));
      if (item) openItemDialog(item);
    }
  }
  for (const sel of ['#itemsTable tbody', '#reorderTable tbody', '#actTable tbody']) {
    const tb = $(sel);
    tb.addEventListener('click', onRowActivate);
    tb.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' && e.target.matches('tr[data-id]')) onRowActivate(e);
    });
  }

  document.addEventListener('click', (e) => {
    const action = e.target.closest('[data-action]')?.dataset.action;
    if (action === 'clear') clearFilters();
    else if (action === 'add') openItemDialog();
    else if (action === 'import') openImport();
    else if (action === 'show-reorder') {
      clearFilters();
      state.status = 'Reorder';
      $('#fStatus').value = 'Reorder';
      setView('inventory');
      renderInventory();
    }
    const bar = e.target.closest('.bar-row');
    if (bar) {
      clearFilters();
      state.category = bar.dataset.category;
      $('#fCategory').value = state.category;
      setView('inventory');
      renderInventory();
    }
    // Close the export menu when clicking elsewhere (or after choosing an option)
    const menu = $('#exportMenu');
    if (menu.open && (!menu.contains(e.target) || e.target.closest('.menu-list a'))) menu.open = false;
  });

  $$('[data-close]').forEach((b) => b.addEventListener('click', () => b.closest('dialog').close()));

  $('#actSearch').addEventListener('input', (e) => { state.actQ = e.target.value; renderActivity(); });
  $('#actKind').addEventListener('change', (e) => { state.actKind = e.target.value; renderActivity(); });

  document.addEventListener('keydown', (e) => {
    const typing = e.target.closest('input, textarea, select, [contenteditable]');
    if (e.key === '/' && !typing && !document.querySelector('dialog[open]')) {
      e.preventDefault();
      setView('inventory', { focus: true });
    }
    if (e.key === 'Escape') {
      const menu = $('#exportMenu');
      if (menu.open) menu.open = false;
    }
  });

  // ------------------------------------------------------------------ start
  (async () => {
    try {
      await loadItems();
    } catch (ex) {
      toast(ex.message, 'error');
    }
    renderMeta();
    renderInventory();
    setView(location.hash.slice(1) || 'inventory');
  })();
})();
