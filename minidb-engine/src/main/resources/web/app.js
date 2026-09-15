const example = `-- MiniDB Web 演示脚本：数据会写入页式数据库并在服务重启后保留。
DROP TABLE IF EXISTS demo_student;
CREATE TABLE demo_student(id INT PRIMARY KEY, name VARCHAR(24) NOT NULL, age INT, active BOOL);
INSERT INTO demo_student VALUES (1, 'Alice', 20, TRUE), (2, 'Tom', 16, FALSE);
SELECT name FROM demo_student WHERE age >= 18;
UPDATE demo_student SET age = age + 1 WHERE id = 1;
DELETE FROM demo_student WHERE id = 2;
SELECT id, name, age, active FROM demo_student;`;

const DEMO_CASES = [
  { group: '目录与约束', title: '建表、目录与完整性约束', summary: '验证表目录、主键、UNIQUE、NOT NULL 与 DEFAULT。', expected: '返回 3 行；id、code 与默认 priority 均被保留。', risk: '会重建 demo_catalog。', sql: `DROP TABLE IF EXISTS demo_catalog;
CREATE TABLE demo_catalog(id INT PRIMARY KEY, code VARCHAR(12) NOT NULL UNIQUE, priority INT DEFAULT 1, enabled BOOL NOT NULL DEFAULT TRUE);
INSERT INTO demo_catalog(id, code) VALUES (1, 'alpha'), (2, 'beta');
INSERT INTO demo_catalog VALUES (3, 'gamma', 3, FALSE);
SELECT id, code, priority, enabled FROM demo_catalog ORDER BY id;` },
  { group: '行编码', title: 'INT、FLOAT、VARCHAR、BOOL、NULL', summary: '验证页式行存储对全部逻辑类型和 NULL 的编码、扫描与读取。', expected: 'NULL 会以斜体 NULL 显示，浮点与布尔值保持原值。', risk: '会重建 demo_types。', sql: `DROP TABLE IF EXISTS demo_types;
CREATE TABLE demo_types(id INT PRIMARY KEY, label VARCHAR(24), score FLOAT, active BOOL, remark VARCHAR(32));
INSERT INTO demo_types VALUES (1, 'integer-float', 91.5, TRUE, NULL), (2, 'nullable', NULL, FALSE, 'stored in a data page');
UPDATE demo_types SET remark = 'updated row' WHERE id = 1;
SELECT id, label, score, active, remark FROM demo_types ORDER BY id;` },
  { group: 'DML', title: 'INSERT、UPDATE、DELETE', summary: '验证插入、表达式更新、删除标记与顺序扫描。', expected: '最终仅显示 id=1 和 id=3，id=1 的 quantity 为 13。', risk: '会重建 demo_mutation。', sql: `DROP TABLE IF EXISTS demo_mutation;
CREATE TABLE demo_mutation(id INT PRIMARY KEY, product VARCHAR(20), quantity INT);
INSERT INTO demo_mutation VALUES (1, 'keyboard', 10), (2, 'mouse', 5), (3, 'screen', 2);
UPDATE demo_mutation SET quantity = quantity + 3 WHERE id = 1;
DELETE FROM demo_mutation WHERE id = 2;
SELECT id, product, quantity FROM demo_mutation ORDER BY id;` },
  { group: '查询执行器', title: '筛选、排序与聚合', summary: '验证 Filter、Sort、Project、GROUP BY、HAVING 与聚合函数。', expected: '按区域聚合，并只保留订单数大于 1 的分组。', risk: '会重建 demo_orders。', sql: `DROP TABLE IF EXISTS demo_orders;
CREATE TABLE demo_orders(id INT PRIMARY KEY, region VARCHAR(10), amount FLOAT, paid BOOL);
INSERT INTO demo_orders VALUES (1, 'east', 12.5, TRUE), (2, 'east', 19.0, TRUE), (3, 'west', 8.0, FALSE), (4, 'west', 21.0, TRUE);
SELECT region, COUNT(*) AS orders, SUM(amount) AS total FROM demo_orders WHERE paid = TRUE GROUP BY region HAVING COUNT(*) >= 1 ORDER BY total DESC;` },
  { group: '高级 SQL', title: 'JOIN、子查询、集合与 CASE', summary: '验证多算子计划及 Java 执行器的复杂查询路径。', expected: '显示 JOIN 结果、IN 子查询结果和 CASE 分类。', risk: '会重建 demo_people 与 demo_scores。', sql: `DROP TABLE IF EXISTS demo_scores, demo_people;
CREATE TABLE demo_people(id INT PRIMARY KEY, name VARCHAR(20), age INT);
CREATE TABLE demo_scores(person_id INT, score FLOAT);
INSERT INTO demo_people VALUES (1, 'Alice', 20), (2, 'Bob', 17), (3, 'Cara', 26);
INSERT INTO demo_scores VALUES (1, 92.0), (1, 85.0), (3, 97.5);
SELECT p.name, s.score FROM demo_people p JOIN demo_scores s ON p.id = s.person_id WHERE s.score >= 90.0 ORDER BY s.score DESC;
SELECT name, CASE WHEN age >= 18 THEN 'adult' ELSE 'minor' END AS category FROM demo_people WHERE id IN (SELECT person_id FROM demo_scores WHERE score >= 90.0) ORDER BY id;
SELECT id FROM demo_people UNION SELECT person_id FROM demo_scores;` },
  { group: '目录演进', title: 'ALTER TABLE 与目录更新', summary: '验证列新增、列重命名、表重命名及持久化目录同步。', expected: '最终查询 demo_schema_v2，且已有行拥有默认 note。', risk: '会重建 demo_schema。', sql: `DROP TABLE IF EXISTS demo_schema_v2, demo_schema;
CREATE TABLE demo_schema(id INT PRIMARY KEY, name VARCHAR(20));
INSERT INTO demo_schema VALUES (1, 'catalog row');
ALTER TABLE demo_schema ADD COLUMN memo VARCHAR(20) NOT NULL DEFAULT 'persisted';
ALTER TABLE demo_schema RENAME COLUMN memo TO note;
ALTER TABLE demo_schema RENAME TO demo_schema_v2;
SELECT id, name, note FROM demo_schema_v2;` },
  { group: '错误边界', title: '约束冲突（预期失败）', summary: '故意插入重复 UNIQUE 值，验证引擎错误不会被误显示为空结果。', expected: '最后一条语句显示 ConstraintViolation；此前的建表与首行插入已完成。', risk: '会重建 demo_error；本用例应以错误卡片结束。', sql: `DROP TABLE IF EXISTS demo_error;
CREATE TABLE demo_error(id INT PRIMARY KEY, email VARCHAR(30) NOT NULL UNIQUE);
INSERT INTO demo_error VALUES (1, 'same@example.test');
INSERT INTO demo_error VALUES (2, 'same@example.test');` },
  { group: '执行计划', title: 'EXPLAIN 与 EXPLAIN ANALYZE', summary: '比较只编译的计划和带实际行数、耗时、循环次数的执行计划。', expected: '两份计划均以等宽文本显示；ANALYZE 标为“已执行”。', risk: '会重建 demo_plan。', sql: `DROP TABLE IF EXISTS demo_plan;
CREATE TABLE demo_plan(id INT PRIMARY KEY, name VARCHAR(20), age INT);
INSERT INTO demo_plan VALUES (1, 'Alice', 20), (2, 'Bob', 17), (3, 'Cara', 25);
EXPLAIN SELECT name FROM demo_plan WHERE age >= 18 ORDER BY id;
EXPLAIN ANALYZE SELECT name FROM demo_plan WHERE age >= 18 ORDER BY id;` },
  { group: '持久化与缓存', title: '重启后验证与缓存观测', summary: '先运行“写入”创建数据；重启服务后再运行“验证”，观察状态栏缓存计数。', expected: '写入后表和数据保存在 data/minidb.db；验证脚本无需重建表。', risk: '写入会重建 demo_persist；验证不会修改数据。', sql: `-- 第一次：运行本脚本写入演示数据。重启 Web 服务后，将下方 SQL 单独运行以验证。
DROP TABLE IF EXISTS demo_persist;
CREATE TABLE demo_persist(id INT PRIMARY KEY, message VARCHAR(30));
INSERT INTO demo_persist VALUES (1, 'survives a normal restart'), (2, 'page-backed row');
SELECT id, message FROM demo_persist ORDER BY id;

-- 重启后验证（复制以下语句单独执行）：
-- SELECT id, message FROM demo_persist ORDER BY id;` }
];

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
const storageNotice = document.querySelector('#storage-notice');
const storageDetail = document.querySelector('#storage-detail');
let history = [];

sql.value = example;
renderDemoCases();
refreshEditor();
sql.addEventListener('input', refreshEditor);
sql.addEventListener('keyup', refreshEditor);
sql.addEventListener('click', refreshEditor);
sql.addEventListener('scroll', () => { lineNumbers.scrollTop = sql.scrollTop; });
document.querySelector('#restore').addEventListener('click', () => { sql.value = example; refreshEditor(); sql.focus(); });
document.querySelector('#clear-output').addEventListener('click', () => { result.className = 'result-empty'; result.innerHTML = '<span class="empty-glyph">↳</span><p>执行输出已清空。</p>'; plan.textContent = '等待下一次成功执行…'; });
execute.addEventListener('click', run);

function renderDemoCases() {
  const host = document.querySelector('#demo-cases');
  DEMO_CASES.forEach((demo, index) => {
    const button = document.createElement('button'); button.type = 'button'; button.className = 'demo-case';
    const top = document.createElement('span'); top.className = 'demo-case-top';
    const indexLabel = document.createElement('small'); indexLabel.textContent = String(index + 1).padStart(2, '0') + ' / ' + demo.group;
    const title = document.createElement('strong'); title.textContent = demo.title; top.append(indexLabel, title);
    const description = document.createElement('span'); description.className = 'demo-description'; description.textContent = demo.summary;
    const expected = document.createElement('span'); expected.className = 'demo-expected'; expected.textContent = '预期：' + demo.expected;
    const risk = document.createElement('span'); risk.className = 'demo-risk'; risk.textContent = demo.risk;
    button.append(top, description, expected, risk);
    button.addEventListener('click', () => { sql.value = demo.sql; refreshEditor(); sql.focus(); button.animate([{ transform: 'translateY(0)' }, { transform: 'translateY(-3px)' }, { transform: 'translateY(0)' }], { duration: 240 }); });
    host.append(button);
  });
}

async function health() {
  try {
    const response = await fetch('/api/health'); const data = await response.json();
    if (!data.compilerAvailable) throw new Error('未找到 C++ 计划导出器');
    applyStorageState(data); statusDot.className = 'dot online';
  } catch (error) {
    statusText.textContent = '引擎不可用 · ' + error.message; statusDot.className = 'dot offline';
    storageNotice.textContent = '无法读取本地持久化存储状态。'; storageDetail.textContent = '请检查 Java Web 服务与 C++ 计划导出器。';
  }
}

async function run() {
  execute.disabled = true; execute.innerHTML = '<span>◌</span> 编译并执行中'; const started = new Date();
  try {
    const response = await fetch('/api/execute', { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({sql:sql.value}) });
    const data = await response.json();
    if (!response.ok || !data.ok) { showError(data); addHistory(started, false, data.code || 'Failed', ''); highlight(data.line, data.column); return; }
    applyStorageState(data); showResults(data.results); plan.textContent = JSON.stringify(data.plan, null, 2);
    addHistory(started, true, `${data.results.length} statements`, extractExplainText(data.results));
  } catch (error) { showError({stage:'network', code:'ConnectionFailed', message:error.message}); addHistory(started, false, 'ConnectionFailed', ''); }
  finally { execute.disabled = false; execute.innerHTML = '<span>▶</span> 运行全部'; }
}

function applyStorageState(data) {
  const storage = data.storage;
  if (!storage || !storage.persistent) {
    statusText.textContent = '测试内存引擎已就绪'; storageNotice.textContent = '当前为测试内存引擎，不代表正式持久化 Web 服务。'; storageDetail.textContent = '无页文件或缓冲池统计。'; return;
  }
  const path = storage.databasePath || 'data/minidb.db';
  statusText.textContent = `持久化引擎已就绪 · ${storage.bufferPolicy} / ${storage.bufferFrames} 帧`;
  storageNotice.textContent = `数据将写入 ${path}；正常关闭并重启服务后仍可读取。`;
  storageDetail.textContent = `数据库：${path}\n缓存：${storage.bufferPolicy} · ${storage.bufferFrames} 帧\n命中 ${storage.hits} · 缺页 ${storage.misses} · 淘汰 ${storage.evictions} · 刷盘 ${storage.flushes}`;
}

function showResults(items) {
  result.className = 'results'; result.replaceChildren();
  items.forEach((item, index) => {
    if (item.kind === 'command') { showCommand(item, index); return; }
    if (isExplainResult(item)) { showExplainResult(item, index); return; }
    showQueryResult(item, index);
  });
}

function showCommand(item, index) {
  const section = document.createElement('section'); section.className = 'command-result';
  section.innerHTML = `<div class="command-title">#${index + 1} · ${escapeHtml(item.operation)}</div><p>影响 <b>${item.affectedRows}</b> 行</p>`; result.append(section);
}

function showQueryResult(item, index) {
  const section = document.createElement('section'); section.className = 'query-result';
  const label = document.createElement('div'); label.className = 'query-label'; label.innerHTML = `<span>#${index + 1} · SELECT RESULT</span><span>${item.rowCount} row(s)</span>`; section.append(label);
  const table = document.createElement('table'); const header = document.createElement('tr');
  item.columns.forEach(column => { const cell = document.createElement('th'); cell.textContent = column; header.append(cell); }); table.append(header);
  item.rows.forEach(row => { const tr = document.createElement('tr'); row.forEach(value => { const cell = document.createElement('td'); cell.textContent = value === null ? 'NULL' : String(value); if (value === null) cell.className = 'null'; tr.append(cell); }); table.append(tr); });
  section.append(table); result.append(section);
}

function isExplainResult(item) { return item.kind === 'query' && item.columns.length === 1 && item.columns[0] === 'QUERY PLAN'; }

function showExplainResult(item, index) {
  const lines = item.rows.map(row => row[0] === null ? 'NULL' : String(row[0])); const analyzed = lines.some(line => /actual rows=\d+/.test(line));
  const section = document.createElement('section'); section.className = `explain-result ${analyzed ? 'analyzed' : 'plain'}`;
  const label = document.createElement('div'); label.className = 'explain-label'; label.innerHTML = `<span>#${index + 1} · ${analyzed ? 'EXPLAIN ANALYZE' : 'EXPLAIN'}</span><span>${analyzed ? '已执行 · 含运行指标' : '仅规划 · 未执行'}</span>`;
  const output = document.createElement('pre'); output.className = 'explain-output'; lines.forEach(line => appendExplainLine(output, line)); section.append(label, output); result.append(section);
}

function appendExplainLine(output, line) {
  const row = document.createElement('span'); row.className = 'explain-line'; const metricAt = line.indexOf('actual rows=');
  if (metricAt < 0) { row.textContent = line; output.append(row); return; }
  const prefix = document.createElement('span'); prefix.textContent = line.slice(0, metricAt);
  const metrics = document.createElement('span'); metrics.className = 'plan-metrics'; metrics.textContent = line.slice(metricAt); row.append(prefix, metrics); output.append(row);
}

function extractExplainText(items) { return items.filter(isExplainResult).flatMap(item => item.rows.map(row => String(row[0]))).join('\n'); }

function showError(data) {
  result.className = ''; result.replaceChildren(); const card = document.createElement('section'); card.className = 'error-card';
  const location = data.line ? ` · line ${data.line}, column ${data.column}` : ''; card.innerHTML = `<b>${escapeHtml((data.stage || 'error').toUpperCase())} / ${escapeHtml(data.code || 'Unknown')}${location}</b><p>${escapeHtml(data.message || '执行失败')}</p>`; result.append(card);
}

function addHistory(time, ok, summary, explainText) {
  history.unshift({time, ok, summary, text:sql.value, explainText}); history = history.slice(0, 8); historyCount.textContent = history.length;
  historyBox.className = 'history-list'; historyBox.replaceChildren();
  history.forEach((entry) => {
    const button = document.createElement('button'); button.className = `history-item ${entry.ok ? 'ok' : 'fail'}`; const marker = entry.explainText ? ' · PLAN' : '';
    button.innerHTML = `<span>${entry.ok ? '✓' : '×'} ${escapeHtml(entry.summary)}${marker}</span><small>${entry.time.toLocaleTimeString()}</small>`;
    button.title = entry.explainText ? '点击恢复此 SQL；该次运行包含文本执行计划。' : '点击恢复此 SQL。';
    button.addEventListener('click', () => { sql.value = entry.text; refreshEditor(); sql.focus(); }); historyBox.append(button);
  });
}

function highlight(line, column) {
  if (!line || !column) return; const lines = sql.value.split('\n'); let offset = 0;
  for (let i = 0; i < line - 1 && i < lines.length; i++) offset += lines[i].length + 1;
  offset += Math.max(0, column - 1); sql.focus(); sql.setSelectionRange(offset, Math.min(offset + 1, sql.value.length));
}

function refreshEditor() {
  const lines = sql.value.split('\n'); lineNumbers.textContent = lines.map((_, index) => index + 1).join('\n');
  const before = sql.value.slice(0, sql.selectionStart); const line = before.split('\n').length; const column = before.length - before.lastIndexOf('\n'); cursorInfo.textContent = `Ln ${line}, Col ${column}`;
}
function escapeHtml(value) { const box = document.createElement('div'); box.textContent = String(value); return box.innerHTML; }
health();
