'use strict';

(() => {
  // ------------------------------------------------------------------ helpers
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];

  let moneyFmt = new Intl.NumberFormat('en-US', { style: 'currency', currency: 'USD' });
  function setCurrency(code) {
    try { moneyFmt = new Intl.NumberFormat(undefined, { style: 'currency', currency: code || 'USD' }); }
    catch { moneyFmt = new Intl.NumberFormat('en-US', { style: 'currency', currency: 'USD' }); }
  }
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
    suppliers: [],      // [{name, website}] remembered by the app
    settings: {},       // from /api/settings
    info: {},           // version and file locations, from /api/info
    selectedId: null,   // highlighted row in the inventory list
    pendingFile: null,  // datasheet file chosen in the item dialog, uploaded on save
    removeFile: false,  // remove the attached datasheet on save
    supAutoUrl: '',     // website auto-filled from the supplier name
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
    state.suppliers = data.suppliers || [];
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
  const hasDatasheet = (it) => Boolean(it.datasheet_url || it.datasheet_file);
  const datasheetFileUrl = (it) => `/api/items/${it.id}/datasheet`;

  const ICONS = {
    doc: '<svg viewBox="0 0 20 20" aria-hidden="true"><path d="M6 2.5h5.5L15 6v11.5H6z"/><path d="M11 2.5V6.5h4M8.5 10h4M8.5 13h4"/></svg>',
    clip: '<svg viewBox="0 0 20 20" aria-hidden="true"><path d="M13.5 6.5l-6 6a1.8 1.8 0 002.5 2.5l6.5-6.5a3.2 3.2 0 00-4.5-4.5L5.5 10.5a4.6 4.6 0 006.5 6.5l5-5"/></svg>',
    globe: '<svg viewBox="0 0 20 20" aria-hidden="true"><circle cx="10" cy="10" r="7"/><path d="M3 10h14M10 3c2 2.2 2.8 4.5 2.8 7s-.8 4.8-2.8 7c-2-2.2-2.8-4.5-2.8-7S8 5.2 10 3z"/></svg>',
  };
  function linkIcons(it) {
    const out = [];
    if (it.datasheet_url) out.push(`<a class="row-link" href="${esc(it.datasheet_url)}" target="_blank" rel="noopener noreferrer" title="Open datasheet link">${ICONS.doc}<span class="sr-only">Datasheet</span></a>`);
    if (it.datasheet_file) out.push(`<a class="row-link" href="${datasheetFileUrl(it)}" target="_blank" rel="noopener noreferrer" title="Open attached file: ${esc(it.datasheet_file)}">${ICONS.clip}<span class="sr-only">Datasheet file</span></a>`);
    if (it.supplier_url) out.push(`<a class="row-link" href="${esc(it.supplier_url)}" target="_blank" rel="noopener noreferrer" title="Open ${esc(it.supplier || 'supplier')} website">${ICONS.globe}<span class="sr-only">Supplier website</span></a>`);
    return out.length ? `<span class="row-links">${out.join('')}</span>` : '';
  }

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
      if (state.status === 'NoDatasheet' && hasDatasheet(it)) return false;
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
      <tr data-id="${it.id}" tabindex="0" aria-selected="${it.id === state.selectedId}" class="${it.status === 'Reorder' ? 'is-low' : ''}${it.id === state.selectedId ? ' is-selected' : ''}">
        <td class="mono nowrap">${hl(it.sku, terms)}</td>
        <td class="name"><span class="name-line">${hl(it.name, terms)}${linkIcons(it)}</span>${it.notes ? `<div class="sub">${hl(it.notes.length > 90 ? it.notes.slice(0, 90) + '…' : it.notes, terms)}</div>` : ''}</td>
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
    const noSheet = items.filter((it) => !hasDatasheet(it)).length;

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
      `<button type="button" class="kpi kpi-action ${noSheet ? 'kpi-info' : ''}" data-action="show-nodatasheet">
        <div class="kpi-label">Missing datasheet</div><div class="kpi-value">${fmtNum(noSheet)}</div>
        <div class="kpi-sub">${noSheet ? 'no link or file · View list →' : 'every item has one'}</div></button>`,
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
    const sheet = itemForm.elements.datasheet_url.value.trim() ||
      (state.pendingFile || (state.editing && state.editing.datasheet_file && !state.removeFile));
    $('#itemCalc').innerHTML =
      `<span>Total value <b>${fmtMoney(qty * cost)}</b></span>` +
      (margin !== null ? `<span>Margin <b>${numFmt.format(margin)}%</b></span>` : '') +
      `<span>Status ${status}</span>` +
      `<span>Datasheet ${sheet ? '<span class="badge badge-ok">Added</span>' : '<span class="badge badge-info">Missing</span>'}</span>`;
  }

  // ---- links & attached file in the item dialog
  const LINK_FIELDS = ['supplier_url', 'datasheet_url'];

  /** Same rules as the server: blank, http(s)://..., or a bare address like www.x.com. */
  function normalizeUrl(raw) {
    const s = raw.trim();
    if (!s) return '';
    if (/^https?:\/\//i.test(s)) return s;
    if (/^[a-z][a-z0-9+.-]*:(?!\d)/i.test(s) || !s.includes('.') || /\s/.test(s)) return null;
    return 'https://' + s;
  }

  function updateOpenButtons() {
    for (const f of LINK_FIELDS) {
      const url = normalizeUrl(itemForm.elements[f].value);
      const a = $(`[data-open-for="${f}"]`);
      a.hidden = !url;
      if (url) a.href = url;
    }
  }

  function websiteFor(name) {
    const n = name.trim().toLowerCase();
    if (!n) return '';
    const known = state.suppliers.find((m) => m.name.toLowerCase() === n);
    if (known && known.website) return known.website;
    const item = state.items.find((it) => it.supplier.toLowerCase() === n && it.supplier_url);
    return item ? item.supplier_url : '';
  }

  function autofillWebsite() {
    const field = itemForm.elements.supplier_url;
    // Only fill an empty box, or replace a website we filled in ourselves.
    if (field.value.trim() && field.value !== state.supAutoUrl) return;
    const site = websiteFor(itemForm.elements.supplier.value);
    field.value = site;
    state.supAutoUrl = site;
    $('#supAutoNote').hidden = !site;
    updateOpenButtons();
  }

  function renderAttachment() {
    const item = state.editing;
    const name = $('#attachName');
    const open = $('#attachOpen');
    const remove = $('#attachRemove');
    if (state.pendingFile) {
      name.textContent = `${state.pendingFile.name} (${fmtSize(state.pendingFile.size)}) — will be attached when you save`;
      open.hidden = true;
      remove.hidden = false;
    } else if (item && item.datasheet_file && !state.removeFile) {
      name.textContent = `${item.datasheet_file} (${fmtSize(item.datasheet_size)})`;
      open.href = datasheetFileUrl(item);
      open.hidden = false;
      remove.hidden = false;
    } else {
      name.textContent = state.removeFile ? 'File will be removed when you save' : 'No file attached';
      open.hidden = true;
      remove.hidden = true;
    }
    $('#attachPickLabel').textContent = (state.pendingFile || (item && item.datasheet_file && !state.removeFile)) ? 'Replace…' : 'Attach file…';
    updateItemCalc();
  }

  function fmtSize(bytes) {
    if (bytes >= 1048576) return `${numFmt.format(bytes / 1048576)} MB`;
    if (bytes >= 1024) return `${Math.round(bytes / 1024)} KB`;
    return `${bytes} bytes`;
  }

  $('#attachFile').addEventListener('change', (e) => {
    const file = e.target.files[0];
    e.target.value = '';
    if (!file) return;
    if (file.size > 25 * 1024 * 1024) { showError($('#itemError'), 'That file is larger than 25 MB.'); return; }
    if (file.size === 0) { showError($('#itemError'), 'That file is empty.'); return; }
    showError($('#itemError'), '');
    state.pendingFile = file;
    state.removeFile = false;
    renderAttachment();
  });
  $('#attachRemove').addEventListener('click', () => {
    if (state.pendingFile) state.pendingFile = null;
    else state.removeFile = true;
    renderAttachment();
  });

  // item: edit it. template: add a new item pre-filled from another one (Duplicate).
  async function openItemDialog(item = null, template = null) {
    state.editing = item;
    itemForm.reset();
    showError($('#itemError'), '');
    $('#itemDialogTitle').textContent = item ? `Edit ${item.sku}` : template ? `Duplicate ${template.sku}` : 'Add item';
    $('#saveItemBtn').textContent = item ? 'Save changes' : 'Add item';
    $('#deleteItemBtn').hidden = !item;
    $('#adjustFromItemBtn').hidden = !item;
    state.pendingFile = null;
    state.removeFile = false;
    state.supAutoUrl = '';
    $('#supAutoNote').hidden = true;
    const FIELDS = ['sku', 'name', 'category', 'location', 'supplier', 'notes', ...LINK_FIELDS, ...NUM_FIELDS];
    if (item) {
      for (const f of FIELDS) itemForm.elements[f].value = item[f] ?? '';
    } else if (template) {
      for (const f of FIELDS) if (f !== 'sku' && f !== 'quantity') itemForm.elements[f].value = template[f] ?? '';
    } else {
      // New item: defaults from Settings, or the category currently being viewed.
      itemForm.elements.category.value = state.category || state.settings.default_category || '';
      const lvl = state.settings.default_reorder_level;
      if (lvl) itemForm.elements.reorder_level.value = lvl;
    }
    updateOpenButtons();
    renderAttachment();
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
    const name = e.target.name;
    if (NUM_FIELDS.includes(name)) updateItemCalc();
    if (name === 'supplier') autofillWebsite();
    if (LINK_FIELDS.includes(name)) {
      if (name === 'supplier_url') $('#supAutoNote').hidden = true;
      updateOpenButtons();
      updateItemCalc();
    }
  });
  // Tidy a bare address ("www.x.com") into a full link when leaving the box.
  for (const f of LINK_FIELDS) {
    itemForm.elements[f].addEventListener('blur', (e) => {
      const url = normalizeUrl(e.target.value);
      if (url) e.target.value = url;
      updateOpenButtons();
    });
  }

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
    for (const f of LINK_FIELDS) {
      const url = normalizeUrl(data[f]);
      if (url === null) {
        const label = f === 'datasheet_url' ? 'Datasheet link' : 'Supplier website';
        showError(err, `${label} must be a web address, like https://www.example.com`);
        itemForm.elements[f].focus();
        return;
      }
      data[f] = url;
    }
    const btn = $('#saveItemBtn');
    btn.disabled = true;
    try {
      const editing = state.editing;
      const res = editing
        ? await api('PUT', `/api/items/${editing.id}`, data)
        : await api('POST', '/api/items', data);
      let fileProblem = '';
      try {
        if (state.pendingFile) {
          await api('POST', `/api/items/${res.item.id}/datasheet?filename=${encodeURIComponent(state.pendingFile.name)}`, state.pendingFile);
        } else if (state.removeFile && editing && editing.datasheet_file) {
          await api('DELETE', `/api/items/${res.item.id}/datasheet`);
        }
      } catch (fx) {
        fileProblem = fx.message;
      }
      $('#itemDialog').close();
      await loadItems();
      renderAll();
      if (fileProblem) toast(`Saved ${res.item.sku}, but the datasheet file wasn't saved: ${fileProblem}`, 'error');
      else toast(editing ? `Saved ${res.item.sku}` : `Added ${res.item.sku} · ${res.item.name}`);
    } catch (ex) {
      showError(err, ex.message);
    } finally {
      btn.disabled = false;
    }
  });

  $('#deleteItemBtn').addEventListener('click', () => { if (state.editing) deleteItem(state.editing, true); });

  async function deleteItem(item, fromDialog = false) {
    const ok = await confirmBox({
      title: `Delete ${item.sku}?`,
      message: `“${item.name}” will be removed from your inventory. Its past stock changes stay in the Activity log.`,
      ok: 'Delete item', danger: true,
    });
    if (!ok) return;
    try {
      await api('DELETE', `/api/items/${item.id}`);
      if (fromDialog) $('#itemDialog').close();
      if (state.selectedId === item.id) state.selectedId = null;
      await loadItems();
      renderAll();
      toast(`Deleted ${item.sku}`);
    } catch (ex) {
      if (fromDialog) showError($('#itemError'), ex.message);
      else toast(ex.message, 'error');
    }
  }

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

  // ------------------------------------------------------------------ selecting & opening items
  // Click selects a row, double-click (or Enter) opens it, right-click shows the item menu.
  const rowItem = (tr) => (tr ? findItem(Number(tr.dataset.id)) : null);
  const selectedItem = () => (state.selectedId != null ? findItem(state.selectedId) : null);

  function selectRow(tr, { focus = false } = {}) {
    const tbody = tr.closest('tbody');
    $$('tr.is-selected', tbody).forEach((r) => { r.classList.remove('is-selected'); r.setAttribute('aria-selected', 'false'); });
    tr.classList.add('is-selected');
    tr.setAttribute('aria-selected', 'true');
    if (tbody.closest('#itemsTable')) state.selectedId = Number(tr.dataset.id);
    if (focus) tr.focus();
  }

  for (const sel of ['#itemsTable tbody', '#reorderTable tbody', '#actTable tbody']) {
    const tb = $(sel);
    tb.addEventListener('click', (e) => {
      if (e.target.closest('a')) return;  // link icons open their page
      const tr = e.target.closest('tr[data-id]');
      if (!tr) return;
      if (e.target.closest('button[data-action]')) {
        const item = rowItem(tr);
        if (item) openAdjust(item, 'add');
        return;
      }
      selectRow(tr);
    });
    tb.addEventListener('dblclick', (e) => {
      if (e.target.closest('a, button')) return;
      const item = rowItem(e.target.closest('tr[data-id]'));
      if (item) openItemDialog(item);
    });
    tb.addEventListener('keydown', (e) => {
      const tr = e.target.closest('tr[data-id]');
      if (!tr || e.target !== tr) return;
      if (e.key === 'Enter') {
        e.preventDefault();
        const item = rowItem(tr);
        if (item) openItemDialog(item);
      } else if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
        e.preventDefault();
        const step = (r) => (e.key === 'ArrowDown' ? r.nextElementSibling : r.previousElementSibling);
        let next = step(tr);
        while (next && !next.matches('tr[data-id]')) next = step(next);
        if (next) selectRow(next, { focus: true });
      } else if (e.key === 'Delete') {
        e.preventDefault();
        const item = rowItem(tr);
        if (item) deleteItem(item);
      } else if (e.key === 'ContextMenu' || (e.shiftKey && e.key === 'F10')) {
        e.preventDefault();
        const r = tr.getBoundingClientRect();
        selectRow(tr);
        showItemMenu(rowItem(tr), r.left + 40, r.bottom - 4);
      }
    });
    tb.addEventListener('focusin', (e) => {
      const tr = e.target.closest('tr[data-id]');
      if (tr && e.target === tr) selectRow(tr);
    });
  }

  // ------------------------------------------------------------------ menus (right-click and menu bar)
  const ctx = $('#ctxMenu');
  let ctxItems = [];
  let menuOwner = null;  // the menu-bar button whose menu is open, if any

  /** items: [{ label, shortcut, action, disabled, danger } | 'sep'] shown at (x, y). */
  function showMenu(items, x, y, owner = null) {
    closeMenu();
    ctxItems = items;
    menuOwner = owner;
    ctx.innerHTML = items.map((it, i) => (it === 'sep'
      ? '<div class="ctx-sep" role="separator"></div>'
      : `<button type="button" role="menuitem" class="ctx-item${it.danger ? ' danger' : ''}" data-i="${i}"${it.disabled ? ' disabled' : ''}>
          <span>${esc(it.label)}</span>${it.shortcut ? `<kbd>${esc(it.shortcut)}</kbd>` : ''}</button>`)).join('');
    ctx.hidden = false;
    // Keep the menu on screen.
    const w = ctx.offsetWidth;
    const h = ctx.offsetHeight;
    ctx.style.left = `${Math.max(4, Math.min(x, innerWidth - w - 4))}px`;
    ctx.style.top = `${Math.max(4, Math.min(y, innerHeight - h - 4))}px`;
    if (owner) { owner.setAttribute('aria-expanded', 'true'); owner.classList.add('open'); }
    ctx.querySelector('.ctx-item:not([disabled])')?.focus();
  }

  function closeMenu() {
    if (ctx.hidden) return;
    ctx.hidden = true;
    ctx.innerHTML = '';
    if (menuOwner) { menuOwner.setAttribute('aria-expanded', 'false'); menuOwner.classList.remove('open'); }
    menuOwner = null;
  }

  ctx.addEventListener('click', (e) => {
    const b = e.target.closest('.ctx-item');
    if (!b || b.disabled) return;
    const it = ctxItems[Number(b.dataset.i)];
    closeMenu();
    it.action();
  });
  ctx.addEventListener('keydown', (e) => {
    const items = $$('.ctx-item:not([disabled])', ctx);
    const i = items.indexOf(document.activeElement);
    if (e.key === 'ArrowDown') { e.preventDefault(); items[(i + 1) % items.length]?.focus(); }
    else if (e.key === 'ArrowUp') { e.preventDefault(); items[(i - 1 + items.length) % items.length]?.focus(); }
    else if (e.key === 'Escape') { e.preventDefault(); const o = menuOwner; closeMenu(); o?.focus(); }
    else if ((e.key === 'ArrowRight' || e.key === 'ArrowLeft') && menuOwner) {
      e.preventDefault();
      openMenubar(stepMenubar(menuOwner, e.key === 'ArrowRight' ? 1 : -1));
    }
  });
  document.addEventListener('mousedown', (e) => {
    if (!ctx.hidden && !ctx.contains(e.target) && !e.target.closest('.menubar-item')) closeMenu();
  });
  window.addEventListener('blur', closeMenu);
  window.addEventListener('resize', closeMenu);
  document.addEventListener('scroll', (e) => { if (!ctx.contains(e.target)) closeMenu(); }, true);

  // Opens a web page or file in the normal web browser (from the app window) or a new tab.
  const openLink = (url) => { if (url) window.open(url, '_blank', 'noopener'); };

  function showItemMenu(item, x, y) {
    if (!item) return;
    showMenu([
      { label: 'Edit item…', shortcut: 'Enter', action: () => openItemDialog(item) },
      { label: 'Receive stock…', action: () => openAdjust(item, 'add') },
      { label: 'Remove stock…', action: () => openAdjust(item, 'remove') },
      { label: 'Set count…', action: () => openAdjust(item, 'set') },
      'sep',
      { label: 'Open datasheet link', disabled: !item.datasheet_url, action: () => openLink(item.datasheet_url) },
      { label: 'Open datasheet file', disabled: !item.datasheet_file, action: () => openLink(datasheetFileUrl(item)) },
      { label: 'Open supplier website', disabled: !item.supplier_url, action: () => openLink(item.supplier_url) },
      'sep',
      { label: 'Duplicate item…', action: () => openItemDialog(null, item) },
      { label: 'Delete item…', shortcut: 'Del', danger: true, action: () => deleteItem(item) },
      'sep',
      { label: 'Add new item…', shortcut: 'Ctrl+N', action: () => openItemDialog() },
    ], x, y);
  }

  function showAreaMenu(x, y) {
    showMenu([
      { label: 'Add item…', shortcut: 'Ctrl+N', action: () => openItemDialog() },
      { label: 'Import from spreadsheet…', shortcut: 'Ctrl+I', action: openImport },
      'sep',
      { label: 'Clear search & filters', disabled: !isFiltered(), action: clearFilters },
    ], x, y);
  }

  document.addEventListener('contextmenu', (e) => {
    // Text boxes keep the normal cut/copy/paste menu.
    if (e.target.closest('input, textarea, select, [contenteditable]')) return;
    e.preventDefault();
    if (document.querySelector('dialog[open]') || ctx.contains(e.target)) return;
    const tr = e.target.closest('tr[data-id]');
    if (tr) {
      selectRow(tr);
      showItemMenu(rowItem(tr), e.clientX, e.clientY);
    } else if (state.view === 'inventory' && e.target.closest('main')) {
      showAreaMenu(e.clientX, e.clientY);
    }
  });

  // ---- menu bar
  const inApp = Boolean(window.chrome && window.chrome.webview);  // running in the app window

  function download(url) {
    const a = document.createElement('a');
    a.href = url;
    a.download = '';
    document.body.append(a);
    a.click();
    a.remove();
  }
  function backupNow() {
    download('/api/backup');
    if (!inApp) toast('Backup file downloading — keep it somewhere safe.');
  }
  function showStatus(status) {
    clearFilters();
    state.status = status;
    $('#fStatus').value = status;
    setView('inventory');
    renderInventory();
  }

  const MENUS = {
    file: () => [
      { label: 'New item…', shortcut: 'Ctrl+N', action: () => openItemDialog() },
      'sep',
      { label: 'Import from spreadsheet…', shortcut: 'Ctrl+I', action: openImport },
      { label: 'Export to Excel…', shortcut: 'Ctrl+E', action: () => download('/api/export.xlsx') },
      { label: 'Export to CSV…', action: () => download('/api/export.csv') },
      'sep',
      { label: 'Back up inventory…', shortcut: 'Ctrl+B', action: backupNow },
      { label: 'Restore from backup…', action: openRestore },
      'sep',
      { label: 'Settings…', shortcut: 'Ctrl+,', action: openSettings },
      ...(inApp ? ['sep', { label: 'Exit', shortcut: 'Alt+F4', action: () => window.chrome.webview.postMessage('exit') }] : []),
    ],
    edit: () => {
      const it = selectedItem();
      return [
        { label: 'Add item…', shortcut: 'Ctrl+N', action: () => openItemDialog() },
        { label: 'Edit selected item…', shortcut: 'Enter', disabled: !it, action: () => openItemDialog(it) },
        { label: 'Adjust stock of selected item…', disabled: !it, action: () => openAdjust(it, 'add') },
        { label: 'Duplicate selected item…', disabled: !it, action: () => openItemDialog(null, it) },
        { label: 'Delete selected item…', shortcut: 'Del', disabled: !it, danger: true, action: () => deleteItem(it) },
        'sep',
        { label: 'Find…', shortcut: 'Ctrl+F', action: () => setView('inventory', { focus: true }) },
      ];
    },
    view: () => [
      { label: 'Inventory', shortcut: 'Ctrl+1', action: () => setView('inventory') },
      { label: 'Dashboard', shortcut: 'Ctrl+2', action: () => setView('dashboard') },
      { label: 'Activity', shortcut: 'Ctrl+3', action: () => setView('activity') },
      'sep',
      { label: 'Items needing reorder', action: () => showStatus('Reorder') },
      { label: 'Items missing a datasheet', action: () => showStatus('NoDatasheet') },
      { label: 'Clear search & filters', disabled: !isFiltered(), action: () => { clearFilters(); setView('inventory'); } },
    ],
    help: () => [
      { label: 'Keyboard and mouse shortcuts', action: () => $('#shortcutsDialog').showModal() },
      { label: 'Open data folder', action: () => openFolder('data') },
      'sep',
      { label: 'About Inventory Manager', action: openAbout },
    ],
  };

  const menubarButtons = $$('.menubar-item');
  function openMenubar(btn) {
    const r = btn.getBoundingClientRect();
    showMenu(MENUS[btn.dataset.menu](), r.left, r.bottom, btn);
  }
  function stepMenubar(btn, d) {
    const i = menubarButtons.indexOf(btn);
    return menubarButtons[(i + d + menubarButtons.length) % menubarButtons.length];
  }
  menubarButtons.forEach((b) => {
    b.addEventListener('click', () => { if (menuOwner === b) closeMenu(); else openMenubar(b); });
    b.addEventListener('mouseenter', () => { if (menuOwner && menuOwner !== b) openMenubar(b); });
  });

  // Underline the menu letters while Alt is held, like other Windows programs.
  document.addEventListener('keydown', (e) => { if (e.key === 'Alt') document.body.classList.add('show-access-keys'); });
  document.addEventListener('keyup', (e) => { if (e.key === 'Alt') document.body.classList.remove('show-access-keys'); });
  window.addEventListener('blur', () => document.body.classList.remove('show-access-keys'));

  function handleShortcut(e) {
    if (e.altKey && !e.ctrlKey && !e.shiftKey && !e.metaKey) {
      const m = { f: 'file', e: 'edit', v: 'view', h: 'help' }[e.key.toLowerCase()];
      if (m && !document.querySelector('dialog[open]')) {
        e.preventDefault();
        openMenubar($(`.menubar-item[data-menu="${m}"]`));
      }
      return;
    }
    if (!(e.ctrlKey || e.metaKey) || e.altKey || document.querySelector('dialog[open]')) return;
    const actions = {
      n: () => openItemDialog(), f: () => setView('inventory', { focus: true }), i: openImport,
      e: () => download('/api/export.xlsx'), b: backupNow, ',': openSettings,
      1: () => setView('inventory'), 2: () => setView('dashboard'), 3: () => setView('activity'),
    };
    const fn = actions[e.key.toLowerCase()];
    if (fn) { e.preventDefault(); closeMenu(); fn(); }
  }

  // ------------------------------------------------------------------ settings
  const CURRENCIES = [
    ['USD', 'US dollar'], ['EUR', 'Euro'], ['GBP', 'British pound'], ['CAD', 'Canadian dollar'],
    ['AUD', 'Australian dollar'], ['NZD', 'New Zealand dollar'], ['CHF', 'Swiss franc'], ['JPY', 'Japanese yen'],
    ['CNY', 'Chinese yuan'], ['INR', 'Indian rupee'], ['MXN', 'Mexican peso'], ['BRL', 'Brazilian real'],
    ['ZAR', 'South African rand'], ['SEK', 'Swedish krona'], ['NOK', 'Norwegian krone'], ['DKK', 'Danish krone'],
    ['PLN', 'Polish złoty'], ['SGD', 'Singapore dollar'], ['HKD', 'Hong Kong dollar'], ['AED', 'UAE dirham'],
  ];

  function applyTheme(theme) {
    if (theme === 'light' || theme === 'dark') document.documentElement.dataset.theme = theme;
    else delete document.documentElement.dataset.theme;
    try { localStorage.setItem('im-theme', theme || 'system'); } catch { /* not essential */ }
  }

  function applySettings(s) {
    state.settings = s || {};
    setCurrency(state.settings.currency);
    applyTheme(state.settings.theme);
  }

  const settingsForm = $('#settingsForm');

  async function openSettings() {
    showError($('#settingsError'), '');
    try {
      const [s, info] = await Promise.all([api('GET', '/api/settings'), api('GET', '/api/info')]);
      state.info = info;
      applySettings(s);
    } catch (ex) {
      toast(ex.message, 'error');
      return;
    }
    const s = state.settings;
    const f = settingsForm.elements;
    $('#setCurrency').innerHTML = CURRENCIES.map(([c, n]) => `<option value="${c}">${c} — ${esc(n)}</option>`).join('');
    const cats = [...new Set([...state.defaults, ...distinct('category')])].sort(collator.compare);
    if (s.default_category && !cats.includes(s.default_category)) cats.push(s.default_category);
    $('#setCategory').innerHTML = '<option value="">(none)</option>' +
      cats.map((c) => `<option value="${esc(c)}">${esc(c)}</option>`).join('');
    f.currency.value = s.currency || 'USD';
    f.theme.value = s.theme || 'system';
    f.default_category.value = s.default_category || '';
    f.default_reorder_level.value = s.default_reorder_level ?? 0;
    f.auto_backup.checked = s.auto_backup !== false;
    f.backup_keep.value = s.backup_keep ?? 10;
    $('#setDataPath').textContent = state.info.data_file || '';
    $('#setBackupPath').textContent = state.info.backups_folder || '';
    $('#settingsDialog').showModal();
  }

  // Preview the appearance while choosing it; cancelling puts the saved one back.
  settingsForm.elements.theme.addEventListener('change', (e) => applyTheme(e.target.value));
  $('#settingsDialog').addEventListener('close', () => applyTheme(state.settings.theme));

  settingsForm.addEventListener('submit', async (e) => {
    e.preventDefault();
    const f = settingsForm.elements;
    const body = {
      currency: f.currency.value,
      theme: f.theme.value,
      default_category: f.default_category.value,
      default_reorder_level: f.default_reorder_level.value === '' ? 0 : f.default_reorder_level.value,
      auto_backup: f.auto_backup.checked,
      backup_keep: f.backup_keep.value,
    };
    try {
      applySettings(await api('PUT', '/api/settings', body));
      $('#settingsDialog').close();
      renderAll();
      toast('Settings saved');
    } catch (ex) {
      showError($('#settingsError'), ex.message);
    }
  });

  async function openFolder(which) {
    try {
      await api('POST', `/api/open-folder?which=${which}`);
    } catch (ex) {
      toast(ex.message, 'error');
    }
  }

  // ------------------------------------------------------------------ restore & about
  async function openRestore() {
    showError($('#restoreError'), '');
    $('#restoreFile').value = '';
    const list = $('#backupList');
    list.innerHTML = '<li class="muted">Loading…</li>';
    $('#restoreDialog').showModal();
    try {
      const { backups } = await api('GET', '/api/backups');
      const kindText = { auto: 'automatic', 'before-restore': 'saved before a restore', other: 'backup' };
      list.innerHTML = backups.length ? backups.slice(0, 30).map((b) => `
        <li>
          <div><b>${esc(new Date(b.modified).toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' }))}</b>
            <span class="muted"> · ${kindText[b.kind] || 'backup'} · ${fmtSize(b.size)}</span></div>
          <button type="button" class="btn btn-sm" data-restore="${esc(b.name)}">Restore</button>
        </li>`).join('')
        : '<li class="muted">No backups in the Backups folder yet.</li>';
    } catch (ex) {
      list.innerHTML = '';
      showError($('#restoreError'), ex.message);
    }
  }

  async function doRestore(label, send) {
    const ok = await confirmBox({
      title: 'Restore this backup?',
      message: `Everything in Inventory Manager will be replaced with the backup from ${label}. Your current inventory is saved to the Backups folder first, so this can be undone.`,
      ok: 'Restore backup', danger: true,
    });
    if (!ok) return;
    try {
      const r = await send();
      $('#restoreDialog').close();
      state.selectedId = null;
      applySettings(await api('GET', '/api/settings'));
      await loadItems();
      renderAll();
      toast(`Backup restored — ${plural(r.items, 'item')}`);
    } catch (ex) {
      showError($('#restoreError'), ex.message);
    }
  }

  $('#backupList').addEventListener('click', (e) => {
    const b = e.target.closest('[data-restore]');
    if (!b) return;
    const label = b.closest('li').querySelector('b').textContent;
    doRestore(label, () => api('POST', `/api/restore?name=${encodeURIComponent(b.dataset.restore)}`));
  });
  $('#restoreFile').addEventListener('change', (e) => {
    const file = e.target.files[0];
    if (!file) return;
    doRestore(`“${file.name}”`, () => api('POST', '/api/restore', file));
  });

  async function openAbout() {
    try { state.info = await api('GET', '/api/info'); } catch { /* show what we have */ }
    $('#aboutVersion').textContent = `Version ${state.info.version || ''}`;
    $('#aboutDialog').showModal();
  }

  document.addEventListener('click', (e) => {
    const action = e.target.closest('[data-action]')?.dataset.action;
    if (action === 'clear') clearFilters();
    else if (action === 'add') openItemDialog();
    else if (action === 'import') openImport();
    else if (action === 'show-reorder') showStatus('Reorder');
    else if (action === 'show-nodatasheet') showStatus('NoDatasheet');
    else if (action === 'open-data-folder') openFolder('data');
    else if (action === 'open-backups-folder') openFolder('backups');
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
    const typing = e.target.closest?.('input, textarea, select, [contenteditable]');
    if (e.key === '/' && !typing && !document.querySelector('dialog[open]')) {
      e.preventDefault();
      setView('inventory', { focus: true });
    }
    if (e.key === 'Escape') {
      const menu = $('#exportMenu');
      if (menu.open) menu.open = false;
    }
    handleShortcut(e);
  });

  // ------------------------------------------------------------------ start
  (async () => {
    try {
      const [settings, info] = await Promise.all([api('GET', '/api/settings'), api('GET', '/api/info')]);
      applySettings(settings);
      state.info = info;
    } catch { /* defaults are fine */ }
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
