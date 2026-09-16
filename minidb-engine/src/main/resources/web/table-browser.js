const tableTree = document.querySelector('#table-tree');
const selectedTableName = document.querySelector('#selected-table-name');
const tableSummary = document.querySelector('#table-summary');
const tableData = document.querySelector('#table-data');
const tableStructure = document.querySelector('#table-structure');
const tablePagination = document.querySelector('#table-pagination');
const state = { activeTable: new URLSearchParams(window.location.search).get('table'), pane: 'data', snapshot: null };

document.querySelector('#refresh-catalog').addEventListener('click', refreshCatalog);
document.querySelector('#data-tab').addEventListener('click', () => switchPane('data'));
document.querySelector('#structure-tab').addEventListener('click', () => switchPane('structure'));
refreshCatalog();

async function refreshCatalog() {
  tableTree.replaceChildren(message('正在刷新表目录', 'tree-message'));
  try { const data = await fetchJson('/api/catalog'); tableTree.replaceChildren(); if (!data.tables.length) { tableTree.append(message('还没有可浏览的表。', 'tree-message')); return; } data.tables.forEach((table) => renderTable(table)); if (state.activeTable) await loadTable(state.activeTable, 0); }
  catch (error) { tableTree.replaceChildren(message(`目录读取失败：${error.message}`, 'tree-message')); }
}
function renderTable(table) { const button = document.createElement('button'); button.type = 'button'; button.className = `browser-table${state.activeTable === table.name ? ' active' : ''}`; const name = document.createElement('strong'); name.textContent = table.name; const meta = document.createElement('small'); meta.textContent = `${table.rowCount} 行 / ${table.columnCount} 列`; button.append(name, meta); button.addEventListener('click', () => loadTable(table.name, 0)); tableTree.append(button); }
async function loadTable(name, offset) {
  state.activeTable = name; state.snapshot = null; window.history.replaceState(null, '', `/table-browser.html?table=${encodeURIComponent(name)}`); selectedTableName.textContent = name; tableSummary.textContent = '正在加载表数据'; tableData.replaceChildren(message('正在读取表数据', 'empty-table')); tableStructure.replaceChildren();
  try { const data = await fetchJson(`/api/tables/${encodeURIComponent(name)}?offset=${offset}&limit=100`); state.snapshot = data; renderSnapshot(); await refreshTreeState(); }
  catch (error) { tableSummary.textContent = '无法读取'; tableData.replaceChildren(errorCard(error.message)); tableStructure.replaceChildren(errorCard(error.message)); tablePagination.replaceChildren(); }
}
async function refreshTreeState() { const data = await fetchJson('/api/catalog'); tableTree.replaceChildren(); data.tables.forEach(renderTable); }
function renderSnapshot() { const { table, preview } = state.snapshot; selectedTableName.textContent = table.name; tableSummary.textContent = `${preview.totalRows} 行  /  ${table.columns.length} 列  /  ${table.indexes.length} 索引`; renderData(preview); renderStructure(table); switchPane(state.pane); }
function renderData(preview) { tableData.replaceChildren(); if (!preview.rows.length) tableData.append(message('此表尚无记录。', 'empty-table')); else tableData.append(dataGrid(preview.columns, preview.rows)); tablePagination.replaceChildren(); const first = preview.totalRows ? preview.offset + 1 : 0; const last = Math.min(preview.offset + preview.rows.length, preview.totalRows); const info = document.createElement('span'); info.textContent = `${first}-${last} / ${preview.totalRows} 行`; const previous = pager('上一页', preview.offset === 0, () => loadTable(state.activeTable, Math.max(0, preview.offset - preview.limit))); const next = pager('下一页', preview.offset + preview.rows.length >= preview.totalRows, () => loadTable(state.activeTable, preview.offset + preview.limit)); tablePagination.append(info, previous, next); }
function renderStructure(table) { tableStructure.replaceChildren(); const columns = section('字段'); const grid = document.createElement('div'); grid.className = 'structure-grid'; table.columns.forEach((column) => { const flags = [column.primaryKey ? 'PRIMARY KEY' : '', column.notNull ? 'NOT NULL' : '', column.unique ? 'UNIQUE' : '', column.hasDefault ? `DEFAULT ${format(column.defaultValue)}` : ''].filter(Boolean).join('  /  ') || '允许 NULL'; const card = document.createElement('div'); card.className = 'column-card'; card.append(text(column.name, 'column-name'), text(column.type === 'VARCHAR' && column.varcharLength ? `VARCHAR(${column.varcharLength})` : column.type, 'column-type'), text(flags, 'column-flags')); grid.append(card); }); columns.append(grid); tableStructure.append(columns); const constraints = section('表级约束'); if (!table.constraints.length) constraints.append(message('无额外表级约束。', 'structure-empty')); table.constraints.forEach((constraint) => constraints.append(row(constraint.kind, constraint.columns.map((ordinal) => table.columns[ordinal]?.name || `#${ordinal}`).join(', ')))); tableStructure.append(constraints); const indexes = section('索引'); if (!table.indexes.length) indexes.append(message('此表没有索引。', 'structure-empty')); table.indexes.forEach((index) => { const column = table.columns.find((item) => item.id === index.columnId); indexes.append(row(index.name, `${column?.name || `#${index.columnId}`} / ${index.keyType}${index.unique ? ' / UNIQUE' : ''}`)); }); tableStructure.append(indexes); }
function switchPane(pane) { state.pane = pane; const data = pane === 'data'; document.querySelector('#data-pane').hidden = !data; document.querySelector('#structure-pane').hidden = data; document.querySelector('#data-tab').classList.toggle('active', data); document.querySelector('#structure-tab').classList.toggle('active', !data); }
function dataGrid(columns, rows) { const table = document.createElement('table'); table.className = 'data-grid'; const head = document.createElement('thead'); const header = document.createElement('tr'); columns.forEach((column) => { const cell = document.createElement('th'); cell.textContent = column; header.append(cell); }); head.append(header); const body = document.createElement('tbody'); rows.forEach((rowValues) => { const row = document.createElement('tr'); rowValues.forEach((value) => { const cell = document.createElement('td'); cell.textContent = format(value); if (value === null) cell.className = 'null'; row.append(cell); }); body.append(row); }); table.append(head, body); return table; }
function pager(label, disabled, action) { const button = document.createElement('button'); button.type = 'button'; button.textContent = label; button.disabled = disabled; button.addEventListener('click', action); return button; }
function section(title) { const element = document.createElement('section'); element.className = 'structure-section'; element.append(text(title, 'structure-title')); return element; }
function row(left, right) { const element = document.createElement('div'); element.className = 'structure-row'; element.append(text(left, ''), text(right, '')); return element; }
function errorCard(value) { return text(`表浏览失败：${value}`, 'error-card'); }
function message(value, className) { return text(value, className); }
function text(value, className) { const element = document.createElement('p'); element.className = className; element.textContent = value; return element; }
function format(value) { return value === null ? 'NULL' : String(value); }
async function fetchJson(url) { const response = await fetch(url); const data = await response.json(); if (!response.ok || !data.ok) throw new Error(data.message || '请求失败'); return data; }
