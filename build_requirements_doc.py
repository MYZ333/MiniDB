from pathlib import Path
from docx import Document
from docx.shared import Inches, Pt, RGBColor
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.enum.table import WD_TABLE_ALIGNMENT, WD_CELL_VERTICAL_ALIGNMENT
from docx.oxml import OxmlElement
from docx.oxml.ns import qn

OUT = Path('output') / 'MiniDB_软件需求规格说明书.docx'

NAVY = '17365D'
BLUE = '1F4E78'
LIGHT = 'D9EAF7'
PALE = 'EEF4FA'
GRAY = '666666'

def set_cell_shading(cell, fill):
    tcPr = cell._tc.get_or_add_tcPr()
    shd = tcPr.find(qn('w:shd'))
    if shd is None:
        shd = OxmlElement('w:shd')
        tcPr.append(shd)
    shd.set(qn('w:fill'), fill)

def set_cell_border(cell, color='B7C9DC'):
    tcPr = cell._tc.get_or_add_tcPr()
    borders = tcPr.first_child_found_in('w:tcBorders')
    if borders is None:
        borders = OxmlElement('w:tcBorders')
        tcPr.append(borders)
    for edge in ('top', 'left', 'bottom', 'right'):
        tag = qn(f'w:{edge}')
        el = borders.find(tag)
        if el is None:
            el = OxmlElement(f'w:{edge}')
            borders.append(el)
        el.set(qn('w:val'), 'single')
        el.set(qn('w:sz'), '4')
        el.set(qn('w:color'), color)

def set_repeat_table_header(row):
    trPr = row._tr.get_or_add_trPr()
    tblHeader = OxmlElement('w:tblHeader')
    tblHeader.set(qn('w:val'), 'true')
    trPr.append(tblHeader)

def set_font(run, size=10.5, bold=False, color=None, name='Microsoft YaHei'):
    run.font.name = name
    run._element.rPr.rFonts.set(qn('w:eastAsia'), name)
    run._element.rPr.rFonts.set(qn('w:ascii'), name)
    run._element.rPr.rFonts.set(qn('w:hAnsi'), name)
    run.font.size = Pt(size)
    run.bold = bold
    if color:
        run.font.color.rgb = RGBColor.from_string(color)

def add_text(doc, text, style=None, bold_lead=None):
    p = doc.add_paragraph(style=style)
    p.paragraph_format.space_after = Pt(5)
    p.paragraph_format.line_spacing = 1.35
    if bold_lead and text.startswith(bold_lead):
        set_font(p.add_run(bold_lead), bold=True, color=NAVY)
        set_font(p.add_run(text[len(bold_lead):]))
    else:
        set_font(p.add_run(text))
    return p

def add_bullets(doc, items):
    for item in items:
        p = doc.add_paragraph(style='List Bullet')
        p.paragraph_format.space_after = Pt(2)
        p.paragraph_format.line_spacing = 1.25
        set_font(p.add_run(item))

def add_table(doc, headers, rows, widths=None):
    table = doc.add_table(rows=1, cols=len(headers))
    table.alignment = WD_TABLE_ALIGNMENT.CENTER
    table.style = 'Table Grid'
    hdr = table.rows[0]
    set_repeat_table_header(hdr)
    for i, label in enumerate(headers):
        cell = hdr.cells[i]
        cell.vertical_alignment = WD_CELL_VERTICAL_ALIGNMENT.CENTER
        set_cell_shading(cell, BLUE)
        set_cell_border(cell, '9DB5CC')
        p = cell.paragraphs[0]
        p.alignment = WD_ALIGN_PARAGRAPH.CENTER
        p.paragraph_format.space_after = Pt(0)
        set_font(p.add_run(label), size=9.5, bold=True, color='FFFFFF')
        if widths:
            cell.width = Inches(widths[i])
    for r_idx, row in enumerate(rows):
        cells = table.add_row().cells
        for i, value in enumerate(row):
            cell = cells[i]
            cell.vertical_alignment = WD_CELL_VERTICAL_ALIGNMENT.TOP
            set_cell_border(cell)
            if r_idx % 2 == 0:
                set_cell_shading(cell, PALE)
            p = cell.paragraphs[0]
            p.paragraph_format.space_after = Pt(0)
            p.paragraph_format.line_spacing = 1.15
            set_font(p.add_run(str(value)), size=9.2)
            if widths:
                cell.width = Inches(widths[i])
    doc.add_paragraph().paragraph_format.space_after = Pt(1)
    return table

def heading(doc, text, level=1):
    p = doc.add_paragraph(style=f'Heading {level}')
    p.paragraph_format.space_before = Pt(12 if level == 1 else 8)
    p.paragraph_format.space_after = Pt(5)
    run = p.add_run(text)
    set_font(run, size=15 if level == 1 else 12.5, bold=True, color=NAVY if level == 1 else BLUE)
    return p

def code_block(doc, text):
    p = doc.add_paragraph()
    p.paragraph_format.left_indent = Inches(0.18)
    p.paragraph_format.right_indent = Inches(0.18)
    p.paragraph_format.space_after = Pt(5)
    p.paragraph_format.line_spacing = 1.05
    pPr = p._p.get_or_add_pPr()
    shd = OxmlElement('w:shd')
    shd.set(qn('w:fill'), 'F3F6F8')
    pPr.append(shd)
    set_font(p.add_run(text), size=8.5, name='Consolas')
    return p

def make_doc():
    doc = Document()
    section = doc.sections[0]
    section.top_margin = Inches(0.72)
    section.bottom_margin = Inches(0.68)
    section.left_margin = Inches(0.76)
    section.right_margin = Inches(0.76)

    styles = doc.styles
    styles['Normal'].font.name = 'Microsoft YaHei'
    styles['Normal']._element.rPr.rFonts.set(qn('w:eastAsia'), 'Microsoft YaHei')
    styles['Normal'].font.size = Pt(10.5)
    for name in ('Title', 'Heading 1', 'Heading 2'):
        styles[name].font.name = 'Microsoft YaHei'
        styles[name]._element.rPr.rFonts.set(qn('w:eastAsia'), 'Microsoft YaHei')

    title = doc.add_paragraph(style='Title')
    title.alignment = WD_ALIGN_PARAGRAPH.CENTER
    title.paragraph_format.space_before = Pt(26)
    title.paragraph_format.space_after = Pt(12)
    set_font(title.add_run('MiniDB 软件需求规格说明书'), size=24, bold=True, color='000000')
    sub = doc.add_paragraph()
    sub.alignment = WD_ALIGN_PARAGRAPH.CENTER
    sub.paragraph_format.space_after = Pt(22)
    set_font(sub.add_run('面向四人课程实训小组的可实现需求基线'), size=12, color=GRAY)
    add_table(doc, ['文档属性', '内容'], [
        ('版本', 'V1.0'), ('项目名称', 'MiniDB 简化数据库管理系统'),
        ('目标读者', '项目组成员、课程指导教师'), ('状态', '开发需求基线'),
        ('编制依据', '实习指导书、SQL 编译器课件、数据库系统课件')
    ], [1.35, 5.35])
    add_text(doc, '本说明书定义本组 MiniDB 的功能边界、模块接口、验收条件与四人协作方式。第一阶段以“从 SQL 文本到持久化数据页”的端到端闭环为交付目标，所有实现均应围绕可执行、可测试、可持久化的最小系统展开。')
    doc.add_page_break()

    heading(doc, '1 项目目标与范围')
    add_text(doc, 'MiniDB 是一个教学用途的简化关系型数据库系统。它接收 SQL 文本，经词法分析、语法分析、语义检查和逻辑计划生成后，由执行引擎调用页式存储系统完成数据定义、数据写入、条件查询与删除。系统关闭并重新启动后，表定义和已写入数据必须仍可访问。')
    heading(doc, '1.1 必做范围', 2)
    add_table(doc, ['领域', '必做能力', '可观察结果'], [
        ('SQL 编译器', 'Token、AST、语义检查、逻辑执行计划', '可输出各阶段结果或结构化调试信息'),
        ('SQL 子集', 'CREATE TABLE、INSERT、SELECT ... WHERE、DELETE ... WHERE', '正确执行并返回结果或可定位错误'),
        ('存储系统', '固定页、页分配/释放/读写、缓冲池、替换与刷新', '数据落盘，缓存统计和替换日志可查看'),
        ('数据库引擎', 'CreateTable、Insert、SeqScan、Filter、Project、Delete', '按计划执行并通过存储接口访问数据'),
        ('元数据', '表名、列名、列类型、表页映射', '作为特殊目录表持久化并在重启后恢复'),
        ('交互与测试', 'CLI 或等价 API，自动化测试', '可复现实验链路和错误场景')
    ], [1.2, 3.25, 2.25])
    heading(doc, '1.2 非目标与可选扩展', 2)
    add_text(doc, '下列能力不属于本次验收的前置条件：事务和并发控制、权限控制、崩溃恢复日志、B+ 树索引、JOIN、GROUP BY、ORDER BY、UPDATE、代价优化器和网络服务。核心闭环稳定后，可按优先级选做。', bold_lead='下列能力')
    add_bullets(doc, ['优先扩展：AND、OR、NOT、括号、常量折叠和恒真条件消除。', '后续扩展：UPDATE、ORDER BY、GROUP BY、JOIN、NULL 与更多数据类型。', '性能扩展：索引扫描、谓词下推、投影裁剪、简单代价模型。'])

    heading(doc, '2 用户角色与使用场景')
    add_table(doc, ['角色', '主要动作', '系统反馈'], [
        ('CLI 用户', '输入单条或多条 SQL，查询结果，退出系统', '执行成功信息、结果集或标准化错误'),
        ('开发者', '查看 Token、AST、语义结果、执行计划和缓存日志', '结构化诊断输出，便于定位模块问题'),
        ('测试者', '运行正常、错误、边界和重启持久化测试', '可断言的返回值、日志及持久化结果')
    ], [1.15, 3.45, 2.1])
    heading(doc, '2.1 主成功场景', 2)
    add_bullets(doc, [
        '用户创建 student 表，系统在目录中登记列定义并为该表建立数据页映射。',
        '用户插入多条记录，执行器将行序列化并写入缓冲池中的目标页，刷新后数据进入数据文件。',
        '用户执行带 WHERE 的 SELECT，执行器顺序扫描表页，过滤记录并按 SELECT 列表投影结果。',
        '用户删除满足条件的记录，再次查询时已删除记录不得返回；系统重启后该状态保持一致。'
    ])

    heading(doc, '3 总体架构与数据流')
    add_text(doc, '系统采用分层模块化结构。SQL 编译器只负责将 SQL 转为经过验证的逻辑计划；数据库引擎只执行计划并调用存储接口；存储系统只负责页、文件和缓冲，不理解 SQL 语义。系统目录是两个上层模块共享的元数据来源，最终由数据库引擎经存储系统持久化。')
    code_block(doc, 'SQL 文本 → Lexer → Parser/AST → Semantic/Catalog → Logical Plan\n'
                    '                                              ↓\n'
                    'CLI/API ← 执行结果 ← Executor ← Storage Engine ← Buffer Pool ← Page File')
    add_table(doc, ['层次', '输入', '输出', '不得承担的职责'], [
        ('SQL 编译器', 'SQL 文本、Catalog 快照', 'Token、AST、验证后的 LogicalPlan', '不直接读写数据页'),
        ('数据库引擎', 'LogicalPlan、目录服务', '结果集、影响行数、执行错误', '不解析原始 SQL'),
        ('存储系统', '页号、字节数据、缓冲配置', '页数据、统计、I/O 错误', '不判断表列和 WHERE 条件')
    ], [1.25, 1.75, 2.15, 1.55])

    heading(doc, '4 功能需求')
    heading(doc, '4.1 SQL 编译器', 2)
    add_table(doc, ['编号', '需求', '验收标准'], [
        ('SC-01', '词法分析识别关键字、标识符、整数字面量、字符串、运算符和分隔符；关键字大小写不敏感。', '每个 Token 输出 type、lexeme、line、column。'),
        ('SC-02', '支持空白、单行注释和多行注释；支持字符串中的单引号转义。', '注释不进入 Token 流；未闭合字符串/注释报词法错误。'),
        ('SC-03', '语法分析构造 AST，支持 CREATE TABLE、INSERT、SELECT、DELETE。', '合法语句产生类型正确的 AST，而非仅返回“合法”。'),
        ('SC-04', 'WHERE 支持单个比较表达式；建议在基线中实现 AND、OR、NOT 与括号。', '表达式 AST 保留 NOT > 比较 > AND > OR 的优先级。'),
        ('SC-05', '语义分析检查表和列存在性、INSERT 列数/列序和数据类型。', '失败返回错误类型、位置和原因。'),
        ('SC-06', '执行计划生成 CreateTable、Insert、SeqScan、Filter、Project、Delete 计划节点。', 'SELECT 的计划结构为 Project(Filter(SeqScan)) 或省略恒真 Filter。')
    ], [0.7, 4.05, 1.95])
    heading(doc, '4.2 SQL 语言基线', 2)
    code_block(doc, 'CREATE TABLE table_name (column_name INT|VARCHAR [, ...]);\n'
                    'INSERT INTO table_name (column_name [, ...]) VALUES (value [, ...]);\n'
                    'SELECT * | column_name [, ...] FROM table_name [WHERE expression];\n'
                    'DELETE FROM table_name [WHERE expression];\n'
                    'expression := primary [ (= | != | > | >= | < | <=) primary ]')
    add_text(doc, 'INT 与 VARCHAR 为必做类型。比较运算要求操作数类型兼容。INSERT 允许显式列名列表；列名列表和 VALUES 数量必须相等，未列出的字段暂不支持默认值。每条语句必须以分号结束，输入可包含多条语句。')
    heading(doc, '4.3 逻辑计划格式', 2)
    add_text(doc, '计划采用 JSON 或等价可序列化对象。所有计划节点必须携带 nodeType 和 sourceSpan，便于执行端分发和错误追溯。建议的最小结构如下。')
    code_block(doc, '{"nodeType":"Project","columns":["name"],"child":\n'
                    '  {"nodeType":"Filter","predicate":{"op":">","left":"age","right":18},"child":\n'
                    '    {"nodeType":"SeqScan","table":"student"}}}')
    heading(doc, '4.4 页式存储与缓存', 2)
    add_table(doc, ['编号', '需求', '验收标准'], [
        ('ST-01', '页大小固定为 4 KB，页号在一个数据库文件内唯一。', '读写均以整页为单位，页头可识别页号与页类型。'),
        ('ST-02', '支持 allocate_page、free_page、read_page、write_page。', '分配、释放和重新读取的行为可由单测验证。'),
        ('ST-03', '维护表到页集合的映射，并支持表增长时追加数据页。', '插入超过单页容量后可分配并读取新页。'),
        ('ST-04', '提供缓冲池，默认实现 LRU；FIFO 可作为可切换策略。', '可统计 hit、miss、evict、flush，并记录被替换页号。'),
        ('ST-05', '脏页在显式 flush、淘汰或正常关闭时写回文件。', '关闭重启后已提交到页文件的数据仍可读取。')
    ], [0.7, 4.05, 1.95])
    heading(doc, '4.5 数据库引擎与系统目录', 2)
    add_table(doc, ['编号', '需求', '验收标准'], [
        ('EN-01', '执行器按 nodeType 分发并实现 CreateTable、Insert、SeqScan、Filter、Project、Delete。', '每一种必做 SQL 都可经计划执行。'),
        ('EN-02', '存储引擎负责 Row 与 Page 的序列化、反序列化、插入和扫描。', '扫描结果可还原为带类型的逻辑行。'),
        ('EN-03', '系统目录保存表、列类型和表页映射，并作为特殊表持久化。', '重启后可根据表名恢复结构并继续查询。'),
        ('EN-04', 'DELETE 采用删除标记或物理删除，但查询不得返回已删除行。', '删除后查询、重启后查询均满足预期。'),
        ('EN-05', 'CLI 支持多语句输入、结果格式化、quit/exit 与错误提示。', '用户无需调用内部模块即可完成端到端演示。')
    ], [0.7, 4.05, 1.95])

    heading(doc, '5 关键接口契约')
    add_text(doc, '跨模块调用只能通过以下稳定接口。接口实现语言可调整，但字段含义、错误语义和所有权不得随意改变。')
    add_table(doc, ['接口', '提供方', '使用方', '契约'], [
        ('compile(sql, catalogSnapshot)', 'SQL 编译器', 'CLI、数据库引擎', '成功返回 LogicalPlan 列表；失败返回阶段、位置、原因。'),
        ('get_table_schema(name)', '目录服务', '语义分析、执行器', '返回列名、类型、表标识；不存在返回 NotFound。'),
        ('get_page(id) / unpin_page(id, dirty)', '缓冲池', '存储引擎', 'get_page 返回可读写页；unpin 指定是否产生脏页。'),
        ('read_page(id) / write_page(id, bytes)', '文件管理器', '缓冲池', '读写固定 4 KB；I/O 失败必须可传播。'),
        ('execute(plan)', '执行器', 'CLI', '查询返回列定义和行集；写操作返回状态及影响行数。')
    ], [1.9, 1.25, 1.3, 2.25])
    heading(doc, '5.1 统一错误模型', 2)
    add_table(doc, ['类别', '最小字段', '示例'], [
        ('LexicalError', 'phase, code, line, column, reason', '未闭合字符串'),
        ('SyntaxError', 'phase, line, column, unexpected, expected', '缺少分号或右括号'),
        ('SemanticError', 'phase, line, column, reason', '列不存在或类型不匹配'),
        ('StorageError', 'phase, operation, pageId, reason', '文件读写或页损坏'),
        ('ExecutionError', 'phase, nodeType, reason', '计划节点不支持或记录反序列化失败')
    ], [1.4, 2.85, 2.45])

    heading(doc, '6 数据模型与持久化要求')
    add_table(doc, ['对象', '必备字段', '说明'], [
        ('PageHeader', 'pageId, pageType, freeSpace, slotCount, nextPageId', '每页开头保存，页大小固定 4 KB。'),
        ('DataPage', 'PageHeader, slotDirectory, rowBytes', '槽目录定位行记录，支持顺序扫描。'),
        ('Row', '字段值、删除状态', '按表 Schema 序列化；删除记录不得被扫描返回。'),
        ('TableMeta', 'tableName, columns, firstPageId, pageIds', '目录中保存表结构与物理页映射。'),
        ('CatalogTable', '所有 TableMeta', '特殊系统表；初始化、更新和加载均经存储系统。')
    ], [1.25, 3.05, 2.4])
    add_text(doc, '持久化边界：数据文件、目录表和页分配信息必须位于同一数据库目录或可相互定位的文件集合中。禁止仅在进程内字典保存表结构或用户数据。程序正常关闭时必须 flush 全部脏页；启动时必须加载页分配信息和系统目录。')

    heading(doc, '7 非功能需求')
    add_table(doc, ['编号', '要求', '判定方式'], [
        ('NF-01', '模块边界清晰，SQL 编译器、存储系统、引擎不得相互复制核心逻辑。', '代码审查和接口测试。'),
        ('NF-02', '错误不得导致进程无提示崩溃。', '错误 SQL、错误页号和 I/O 模拟测试。'),
        ('NF-03', '所有核心模块提供单元测试，另有端到端测试脚本。', '一键执行测试并输出结果。'),
        ('NF-04', 'README 说明构建、启动、演示 SQL、目录结构和测试命令。', '他人按文档可复现实验。'),
        ('NF-05', '调试输出可开关，正式结果集不得混入无结构的内部日志。', 'CLI 演示检查。')
    ], [0.75, 4.25, 1.7])

    heading(doc, '8 四人分工与交付边界')
    add_table(doc, ['成员角色', '主要负责', '必须交付', '协作边界'], [
        ('成员 A\nSQL 编译器一', '词法器、Token、文法、递归下降 Parser、AST、语法错误定位。', 'grammar.md、lexer/parser/ast、词法和语法测试。', '输出稳定 AST；不调用数据页。'),
        ('成员 B\nSQL 编译器二', '语义分析、类型规则、编译期 Catalog 适配、Plan 生成、基础优化、编译器集成测试。', 'semantic/planner、计划样例、语义与优化测试。', '通过目录接口读 Schema；不拥有目录持久化。'),
        ('成员 C\n存储系统', '页格式、文件管理、分配/释放、缓冲池、LRU/FIFO、刷新和统计。', 'page/file_manager/buffer、存储单测、缓存日志。', '只处理 pageId 和 bytes；不解释 SQL/Row。'),
        ('成员 D\n数据库引擎', '目录持久化、Row 编解码、存储引擎、执行器、CLI、端到端集成。', 'executor/storage_engine/catalog/cli、集成脚本。', '消费 LogicalPlan；不得重写编译器或缓冲策略。')
    ], [1.15, 2.2, 2.05, 1.3])
    heading(doc, '8.1 集成节奏', 2)
    add_table(doc, ['阶段', '完成条件', '责任'], [
        ('阶段 0 接口冻结', '确定 AST、LogicalPlan、Schema、Page 和错误对象字段；写入接口说明。', '全员'),
        ('阶段 1 模块可测', 'A/B 完成 SQL 到 Plan；C 完成页与缓存；D 完成 Row/Page 与目录的最小闭环。', '各模块负责人'),
        ('阶段 2 冒烟集成', 'CREATE 与 INSERT 可写入页；SELECT 可扫描并显示结果。', 'B+C+D'),
        ('阶段 3 验收闭环', 'DELETE、错误处理、缓存统计与重启持久化全部通过。', '全员')
    ], [1.35, 4.1, 1.25])

    heading(doc, '9 验收测试与完成定义')
    add_table(doc, ['测试类别', '最少覆盖', '通过条件'], [
        ('正常流程', '建表、插入、带条件查询、删除、再查询。', '结果集和影响行数准确。'),
        ('编译错误', '非法字符、未闭合字符串、缺分号、括号错误、未知表/列、类型不匹配、值数量不一致。', '错误阶段和位置正确，进程继续可用。'),
        ('存储与缓存', '页分配/释放、跨页插入、缓存命中、淘汰、脏页刷新。', '页数据与统计值符合预期。'),
        ('持久化', '创建和插入后关闭重启，再查询和继续插入。', '目录和记录均恢复。'),
        ('边界', '空输入、多语句、大小写关键字、长标识符、空结果集。', '行为确定且无崩溃。')
    ], [1.25, 3.7, 1.75])
    heading(doc, '9.1 端到端验收脚本', 2)
    code_block(doc, 'CREATE TABLE student(id INT, name VARCHAR, age INT);\n'
                    "INSERT INTO student(id,name,age) VALUES (1,'Alice',20);\n"
                    "INSERT INTO student(id,name,age) VALUES (2,'Tom',16);\n"
                    'SELECT id,name FROM student WHERE age >= 18;\n'
                    'DELETE FROM student WHERE id = 1;\n'
                    'SELECT * FROM student;\n'
                    '-- 重启 MiniDB 后再次执行 SELECT * FROM student;')
    add_text(doc, '完成定义：上述脚本在首次运行和重启后均产生预期结果；每类错误测试均返回可理解的错误；自动化测试全部通过；README、接口文档和模块测试随源码提交。')

    heading(doc, '10 风险与约束')
    add_table(doc, ['风险', '影响', '控制措施'], [
        ('接口后期变更', '四人模块无法对接。', '阶段 0 冻结对象字段；变更须全员评审并更新测试。'),
        ('目录只存内存', '重启后无法解析表和数据。', '目录表与用户表使用同一存储路径并纳入持久化测试。'),
        ('过早做索引或事务', '核心链路未完成，集成延期。', '扩展功能仅在阶段 3 通过后启动。'),
        ('错误处理分散', '验收时难定位问题。', '采用统一错误对象，阶段名和位置贯穿编译器。'),
        ('行格式与页格式不一致', '插入、扫描和删除行为不稳定。', '由 D 定义 Row 编码，由 C 提供页字节读写，联调测试锁定样例。')
    ], [1.5, 2.25, 2.95])

    heading(doc, '附录 A 文档依据与需求优先级')
    add_text(doc, '本需求基线以课程指导书列出的 SQL 编译器、页式存储与数据库系统核心要求为准，并吸收课件中对 AST、错误定位、逻辑计划、缓存统计、目录持久化和分阶段交付的细化说明。课件中出现的 MySQL 架构、B+ 树、事务与并发等内容作为理解材料或后续扩展，不提升为本组首期必做范围。')
    add_table(doc, ['优先级', '含义'], [
        ('P0 必须', '无法形成端到端可持久化数据库闭环的功能，必须完成。'),
        ('P1 建议', '显著提升编译器质量或演示效果，但不得阻塞 P0。'),
        ('P2 可选', '用于展示扩展能力，只有 P0 和 P1 稳定后实施。')
    ], [1.25, 5.55])

    footer = section.footer.paragraphs[0]
    footer.alignment = WD_ALIGN_PARAGRAPH.CENTER
    footer.paragraph_format.space_before = Pt(6)
    set_font(footer.add_run('MiniDB 软件需求规格说明书  V1.0'), size=8.5, color=GRAY)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    doc.save(OUT)
    print(OUT.resolve())

if __name__ == '__main__':
    make_doc()
