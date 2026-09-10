const example = `-- MiniDB Web 演示脚本：每次运行都会从空数据库开始
CREATE TABLE student(id INT, name VARCHAR, age INT);
INSERT INTO student VALUES (1, 'Alice', 20);
INSERT INTO student VALUES (2, 'Tom', 16);
SELECT name FROM student WHERE age >= 18;
UPDATE student SET age = age + 1 WHERE id = 1;
DELETE FROM student WHERE id = 2;
SELECT id, name, age FROM student;`;

const sql = document.querySelector('#sql');
const execute = document.querySelector('#execute');
const result = document.querySelector('#result');
const plan = document.querySelector('#plan');
const historyBox = document.querySelector('#history');
const historyCount = document.querySelector('#history-count');
const lineNumbers = document.querySelector('#line-numbers');
const cursorInfo = document.querySelector('#cursor-info');
const statusText = document.querySelector('#system-status');
const statusDot = document.querySelector('#status-dot');
let history = [];

sql.value = example;
refreshEditor();
sql.addEventListener('input', refreshEditor);
sql.addEventListener('keyup', refreshEditor);
sql.addEventListener('click', refreshEditor);
sql.addEventListener('scroll', () => { lineNumbers.scrollTop = sql.scrollTop; });
document.querySelector('#restore').addEventListener('click', () => { sql.value = example; refreshEditor(); sql.focus(); });
document.querySelector('#clear-output').addEventListener('click', () => { result.className = 'result-empty'; result.innerHTML = '<span class="empty-glyph">↳</span><p>执行输出已清空。</p>'; plan.textContent = '等待下一次成功执行…'; });
execute.addEventListener('click', run);

async function health() {
  try {
    const response = await fetch('/api/health'); const data = await response.json();
    if (!data.compilerAvailable) throw new Error('未找到 C++ 计划导出器');
    statusText.textContent = '本地引擎已就绪 · 127.0.0.1'; statusDot.className = 'dot online';
  } catch (error) { statusText.textContent = '引擎不可用 · ' + error.message; statusDot.className = 'dot offline'; }
}

async function run() {
  execute.disabled = true; execute.innerHTML = '<span>◌</span> 编译并执行中';
  const started = new Date();
  try {
    const response = await fetch('/api/execute', { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({sql:sql.value}) });
    const data = await response.json();
    if (!response.ok || !data.ok) { showError(data); addHistory(started, false, data.code || 'Failed'); highlight(data.line, data.column); return; }
    showResults(data.results); plan.textContent = JSON.stringify(data.plan, null, 2); addHistory(started, true, `${data.results.length} statements`);
  } catch (error) { showError({stage:'network', code:'ConnectionFailed', message:error.message}); addHistory(started, false, 'ConnectionFailed'); }
  finally { execute.disabled = false; execute.innerHTML = '<span>▶</span> 运行全部'; }
}

function showResults(items) {
  result.className = 'results'; result.replaceChildren();
  items.forEach((item, index) => {
    if (item.kind === 'command') {
      const section = document.createElement('section'); section.className = 'command-result';
      section.innerHTML = `<div class="command-title">#${index + 1} · ${escapeHtml(item.operation)}</div><p>影响 <b>${item.affectedRows}</b> 行</p>`; result.append(section); return;
    }
    const section = document.createElement('section'); section.className = 'query-result';
    const label = document.createElement('div'); label.className = 'query-label'; label.innerHTML = `<span>#${index + 1} · SELECT RESULT</span><span>${item.rowCount} row(s)</span>`; section.append(label);
    const table = document.createElement('table'); const header = document.createElement('tr');
    item.columns.forEach(column => { const cell = document.createElement('th'); cell.textContent = column; header.append(cell); }); table.append(header);
    item.rows.forEach(row => { const tr = document.createElement('tr'); row.forEach(value => { const cell = document.createElement('td'); cell.textContent = value === null ? 'NULL' : String(value); if (value === null) cell.className = 'null'; tr.append(cell); }); table.append(tr); });
    section.append(table); result.append(section);
  });
}

function showError(data) {
  result.className = ''; result.replaceChildren(); const card = document.createElement('section'); card.className = 'error-card';
  const location = data.line ? ` · line ${data.line}, column ${data.column}` : ''; card.innerHTML = `<b>${escapeHtml((data.stage || 'error').toUpperCase())} / ${escapeHtml(data.code || 'Unknown')}${location}</b><p>${escapeHtml(data.message || '执行失败')}</p>`; result.append(card);
}

function addHistory(time, ok, summary) {
  history.unshift({time, ok, summary, text:sql.value}); history = history.slice(0, 8); historyCount.textContent = history.length;
  historyBox.className = 'history-list'; historyBox.replaceChildren();
  history.forEach((entry, index) => { const button = document.createElement('button'); button.className = `history-item ${entry.ok ? 'ok' : 'fail'}`; button.innerHTML = `<span>${entry.ok ? '✓' : '×'} ${escapeHtml(entry.summary)}</span><small>${entry.time.toLocaleTimeString()}</small>`; button.addEventListener('click', () => { sql.value = entry.text; refreshEditor(); sql.focus(); }); historyBox.append(button); });
}

function highlight(line, column) {
  if (!line || !column) return; const lines = sql.value.split('\n'); let offset = 0; for (let i = 0; i < line - 1 && i < lines.length; i++) offset += lines[i].length + 1; offset += Math.max(0, column - 1); sql.focus(); sql.setSelectionRange(offset, Math.min(offset + 1, sql.value.length));
}

function refreshEditor() {
  const lines = sql.value.split('\n'); lineNumbers.textContent = lines.map((_, index) => index + 1).join('\n');
  const before = sql.value.slice(0, sql.selectionStart); const line = before.split('\n').length; const column = before.length - before.lastIndexOf('\n'); cursorInfo.textContent = `Ln ${line}, Col ${column}`;
}
function escapeHtml(value) { const box = document.createElement('div'); box.textContent = String(value); return box.innerHTML; }
health();
