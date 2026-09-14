// A 原有语法测试；合并时补充操作符位置、EOF 和深度/所有权契约。
#include "minisql/lexer.hpp"
#include "minisql/parser.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {
using namespace minisql;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<Statement> parseOk(const std::string& sql) {
    auto lexed = lex(sql);
    if (const auto* error = std::get_if<Diagnostic>(&lexed)) {
        throw std::runtime_error("unexpected lexer error: " + error->message);
    }
    auto parsed = parse(std::get<TokenStream>(lexed));
    if (const auto* error = std::get_if<Diagnostic>(&parsed)) {
        throw std::runtime_error("unexpected parser error: " + error->message);
    }
    return std::get<std::vector<Statement>>(std::move(parsed));
}

void expectSyntaxError(const std::string& sql, ErrorCode code = ErrorCode::UnexpectedToken) {
    auto lexed = lex(sql);
    require(std::holds_alternative<TokenStream>(lexed), "syntax test must first pass lexical analysis");
    auto parsed = parse(std::get<TokenStream>(lexed));
    const auto* error = std::get_if<Diagnostic>(&parsed);
    require(error != nullptr, "expected parser error");
    require(error->stage == DiagnosticStage::Syntax, "expected syntax error stage");
    require(error->code == code, "unexpected parser error code");
    require(error->span.has_value(), "parser error should include source span");
}

void expectSyntaxMessage(const std::string& sql, const std::string& text) {
    auto lexed = lex(sql);
    require(std::holds_alternative<TokenStream>(lexed), "syntax message test must first pass lexical analysis");
    auto parsed = parse(std::get<TokenStream>(lexed));
    const auto* error = std::get_if<Diagnostic>(&parsed);
    require(error != nullptr, "expected parser error");
    require(error->stage == DiagnosticStage::Syntax, "expected syntax error stage");
    require(error->message.find(text) != std::string::npos, "syntax message missing expected text");
    require(error->span.has_value(), "parser error should include source span");
}

const BinaryExpr& asBinary(const ExprPtr& expr, BinaryOp op) {
    const auto* binary = std::get_if<BinaryExpr>(&expr->node);
    require(binary != nullptr, "expected binary expression");
    require(binary->op == op, "unexpected binary operator");
    return *binary;
}

const UnaryExpr& asUnary(const ExprPtr& expr, UnaryOp op) {
    const auto* unary = std::get_if<UnaryExpr>(&expr->node);
    require(unary != nullptr, "expected unary expression");
    require(unary->op == op, "unexpected unary operator");
    return *unary;
}

const IdentifierExpr& asIdentifier(const ExprPtr& expr) {
    const auto* identifier = std::get_if<IdentifierExpr>(&expr->node);
    require(identifier != nullptr, "expected identifier expression");
    return *identifier;
}

const AggregateCall& asAggregate(const SelectItem& item, AggregateFunction function) {
    const auto* aggregate = std::get_if<AggregateCall>(&item);
    require(aggregate != nullptr, "expected aggregate select item");
    require(aggregate->function == function, "unexpected aggregate function");
    return *aggregate;
}

const AggregateCall& asAggregateExpr(const ExprPtr& expr, AggregateFunction function) {
    const auto* aggregate = std::get_if<AggregateCall>(&expr->node);
    require(aggregate != nullptr, "expected aggregate expression");
    require(aggregate->function == function, "unexpected aggregate expression function");
    return *aggregate;
}

const InSubqueryExpr& asInSubquery(const ExprPtr& expr, bool negated) {
    const auto* in_subquery = std::get_if<InSubqueryExpr>(&expr->node);
    require(in_subquery != nullptr, "expected IN subquery expression");
    require(in_subquery->negated == negated, "unexpected IN subquery negation");
    require(in_subquery->query != nullptr, "IN subquery SELECT missing");
    return *in_subquery;
}

const ExistsSubqueryExpr& asExistsSubquery(const ExprPtr& expr, bool negated) {
    const auto* exists_subquery = std::get_if<ExistsSubqueryExpr>(&expr->node);
    require(exists_subquery != nullptr, "expected EXISTS subquery expression");
    require(exists_subquery->negated == negated, "unexpected EXISTS subquery negation");
    require(exists_subquery->query != nullptr, "EXISTS subquery SELECT missing");
    return *exists_subquery;
}

const ScalarSubqueryExpr& asScalarSubquery(const ExprPtr& expr) {
    const auto* scalar_subquery = std::get_if<ScalarSubqueryExpr>(&expr->node);
    require(scalar_subquery != nullptr, "expected scalar subquery expression");
    require(scalar_subquery->query != nullptr, "scalar subquery SELECT missing");
    return *scalar_subquery;
}

const CaseExpr& asCaseExpr(const ExprPtr& expr) {
    const auto* case_expr = std::get_if<CaseExpr>(&expr->node);
    require(case_expr != nullptr, "expected CASE expression");
    return *case_expr;
}

const TableRef& asDerivedTable(const TableRef& source, const char* alias) {
    require(source.subquery != nullptr, "expected derived table subquery");
    require(source.alias && source.alias->text == alias, "derived table alias not retained");
    return source;
}

void testStatements() {
    const auto statements = parseOk(
        "CREATE TABLE student(id INT, name VARCHAR, age INT);"
        "INSERT INTO student(name, age, id) VALUES ('Alice', 20, 1);"
        "INSERT INTO student VALUES (1, 'Tom''s book', 16);"
        "SELECT id, name FROM student WHERE age >= 18;"
        "UPDATE student SET age = age + 1 WHERE id = 1;"
        "DELETE FROM student WHERE id = 1;");
    require(statements.size() == 6, "expected six parsed statements");

    const auto& create = std::get<CreateTableStmt>(statements[0].node);
    require(create.table.text == "student", "unexpected CREATE table");
    require(create.columns.size() == 3, "unexpected CREATE columns");
    require(create.columns[1].name.text == "name" &&
                create.columns[1].type == DataType::Varchar,
            "unexpected CREATE column definition");

    const auto& insert = std::get<InsertStmt>(statements[1].node);
    require(insert.columns.has_value(), "expected explicit INSERT columns");
    require(insert.columns->front().text == "name", "INSERT should retain column order");
    require(std::get<std::string>(insert.values[0].value) == "Alice",
            "expected decoded INSERT string");
    require(insert.rows.size() == 1 && std::get<std::string>(insert.rows[0][0].value) == "Alice",
            "INSERT first row should be mirrored into rows");

    const auto& insertWithoutColumns = std::get<InsertStmt>(statements[2].node);
    require(!insertWithoutColumns.columns.has_value(), "expected omitted INSERT column list");
    require(std::get<std::string>(insertWithoutColumns.values[1].value) == "Tom's book",
            "expected decoded escaped quote");

    const auto& select = std::get<SelectStmt>(statements[3].node);
    require(std::get<std::vector<Identifier>>(select.columns).size() == 2,
            "expected SELECT column list");
    asBinary(select.where, BinaryOp::GreaterEqual);

    const auto& update = std::get<UpdateStmt>(statements[4].node);
    require(update.assignments.size() == 1, "expected one UPDATE assignment");
    asBinary(update.assignments.front().value, BinaryOp::Add);
    asBinary(update.where, BinaryOp::Equal);

    const auto& del = std::get<DeleteStmt>(statements[5].node);
    asBinary(del.where, BinaryOp::Equal);
}

void testMultiRowInsert() {
    const auto statements = parseOk(
        "INSERT INTO student(id, name, age) VALUES "
        "(1, 'Alice', 20), (2, 'Bob', 18), (3, NULL, -1);");
    const auto& insert = std::get<InsertStmt>(statements[0].node);
    require(insert.columns && insert.columns->size() == 3, "multi-row INSERT columns lost");
    require(insert.rows.size() == 3, "multi-row INSERT row count not retained");
    require(insert.values.size() == 3 && std::get<std::int64_t>(insert.values[0].value) == 1,
            "multi-row INSERT first row compatibility values lost");
    require(std::get<std::string>(insert.rows[1][1].value) == "Bob",
            "multi-row INSERT middle row lost");
    require(std::holds_alternative<NullValue>(insert.rows[2][1].value) &&
                std::get<std::int64_t>(insert.rows[2][2].value) == -1,
            "multi-row INSERT NULL or negative literal lost");

    expectSyntaxError("INSERT INTO student VALUES;");
    expectSyntaxError("INSERT INTO student VALUES ();");
    expectSyntaxError("INSERT INTO student VALUES (1),;");
    expectSyntaxError("INSERT INTO student VALUES (1), ();");
}

void testDropTable() {
    const auto statements = parseOk(
        "DROP TABLE student;"
        "DROP TABLE IF EXISTS old_student;"
        "DROP TABLE archive_2024, archive_2025;");
    require(statements.size() == 3, "expected three DROP statements");

    const auto& simple = std::get<DropTableStmt>(statements[0].node);
    require(!simple.if_exists && simple.tables.size() == 1 &&
                simple.tables[0].text == "student",
            "simple DROP TABLE not retained");

    const auto& guarded = std::get<DropTableStmt>(statements[1].node);
    require(guarded.if_exists && guarded.tables.size() == 1 &&
                guarded.tables[0].text == "old_student",
            "DROP TABLE IF EXISTS not retained");

    const auto& multi = std::get<DropTableStmt>(statements[2].node);
    require(!multi.if_exists && multi.tables.size() == 2 &&
                multi.tables[1].text == "archive_2025",
            "DROP TABLE multi-name list not retained");

    expectSyntaxError("DROP student;");
    expectSyntaxError("DROP TABLE;");
    expectSyntaxError("DROP TABLE IF student;");
    expectSyntaxError("DROP TABLE t,;");
}

void testSelectStarAndEmptyInput() {
    const auto empty = parseOk("   -- only comment\n");
    require(empty.empty(), "empty input should parse as zero statements");

    const auto statements = parseOk("SELECT * FROM student;");
    require(statements.size() == 1, "expected one SELECT statement");
    const auto& select = std::get<SelectStmt>(statements[0].node);
    require(std::holds_alternative<AllColumns>(select.columns), "expected SELECT *");
    require(select.where == nullptr, "SELECT without WHERE should have null where");
}

void testDistinctSelect() {
    const auto statements = parseOk(
        "SELECT DISTINCT * FROM student;"
        "SELECT DISTINCT name FROM student;");
    const auto& star = std::get<SelectStmt>(statements[0].node);
    require(star.distinct && std::holds_alternative<AllColumns>(star.columns),
            "SELECT DISTINCT * not retained");
    const auto& column = std::get<SelectStmt>(statements[1].node);
    require(column.distinct &&
                std::get<std::vector<Identifier>>(column.columns)[0].text == "name",
            "SELECT DISTINCT column not retained");
    expectSyntaxError("SELECT name DISTINCT FROM student;");
    expectSyntaxError("SELECT DISTINCT DISTINCT name FROM student;");
}

void testSelectExpressionItems() {
    const auto statements = parseOk(
        "SELECT age + 1 AS next_age, 10 AS ten, active = TRUE AS is_active, "
        "(score) AS grouped_score FROM metrics;");
    const auto& select = std::get<SelectStmt>(statements[0].node);
    const auto& items = std::get<std::vector<SelectItem>>(select.columns);
    require(items.size() == 4, "SELECT expression items not retained");
    const auto& add = asBinary(std::get<ExprPtr>(items[0]), BinaryOp::Add);
    require(asIdentifier(add.left).name.text == "age", "SELECT arithmetic expression left side lost");
    const auto* literal = std::get_if<LiteralExpr>(&std::get<ExprPtr>(items[1])->node);
    require(literal != nullptr && std::get<std::int64_t>(literal->value) == 10,
            "SELECT literal expression lost");
    asBinary(std::get<ExprPtr>(items[2]), BinaryOp::Equal);
    const auto* grouped = std::get_if<IdentifierExpr>(&std::get<ExprPtr>(items[3])->node);
    require(grouped != nullptr && grouped->name.text == "score",
            "parenthesized SELECT expression lost");
    require(select.column_aliases.size() == 4 &&
                select.column_aliases[0] && select.column_aliases[0]->text == "next_age" &&
                select.column_aliases[1] && select.column_aliases[1]->text == "ten" &&
                select.column_aliases[2] && select.column_aliases[2]->text == "is_active" &&
                select.column_aliases[3] && select.column_aliases[3]->text == "grouped_score",
            "SELECT expression aliases not retained");
}

void testExpressionPrecedence() {
    const auto statements =
        parseOk("SELECT * FROM t WHERE a = 1 OR b = 2 AND NOT c = 3;");
    const auto& select = std::get<SelectStmt>(statements[0].node);
    const BinaryExpr& orExpr = asBinary(select.where, BinaryOp::Or);
    asBinary(orExpr.left, BinaryOp::Equal);
    const BinaryExpr& andExpr = asBinary(orExpr.right, BinaryOp::And);
    asBinary(andExpr.left, BinaryOp::Equal);
    const UnaryExpr& notExpr = asUnary(andExpr.right, UnaryOp::Not);
    asBinary(notExpr.operand, BinaryOp::Equal);

    const auto arithmetic = parseOk("SELECT * FROM t WHERE age + 1 * 2 > 18;");
    const auto& arithmeticSelect = std::get<SelectStmt>(arithmetic[0].node);
    const BinaryExpr& greater = asBinary(arithmeticSelect.where, BinaryOp::Greater);
    const BinaryExpr& add = asBinary(greater.left, BinaryOp::Add);
    asBinary(add.right, BinaryOp::Multiply);

    const auto standardNotEqual = parseOk("SELECT * FROM t WHERE id <> 1;");
    const auto& notEqualSelect = std::get<SelectStmt>(standardNotEqual[0].node);
    asBinary(notEqualSelect.where, BinaryOp::NotEqual);
}

void testJoinGroupOrderAndQualifiedNames() {
    const auto statements = parseOk(
        "SELECT student.name FROM student JOIN score ON student.id=score.student_id "
        "JOIN class_info ON student.class_id=class_info.id WHERE score.value>60 "
        "GROUP BY student.name ORDER BY score.value DESC,student.name;");
    const auto& select = std::get<SelectStmt>(statements[0].node);
    require(std::get<std::vector<Identifier>>(select.columns)[0].text == "student.name",
            "qualified projection not retained");
    require(select.joins.size() == 2 && select.joins[0].table.text == "score",
            "JOIN list not retained");
    const auto& on = asBinary(select.joins[0].on, BinaryOp::Equal);
    require(asIdentifier(on.left).name.text == "student.id" &&
            asIdentifier(on.right).name.text == "score.student_id", "qualified ON names lost");
    require(select.group_by.size() == 1 && select.group_by[0].text == "student.name",
            "GROUP BY list not retained");
    require(select.order_by.size() == 2 &&
            select.order_by[0].direction == SortDirection::Desc &&
            select.order_by[1].direction == SortDirection::Asc, "ORDER BY direction mismatch");
}

void testOrderByExpressions() {
    const auto statements = parseOk(
        "SELECT name FROM metrics ORDER BY score + 1 DESC, COUNT(*) ASC;");
    const auto& select = std::get<SelectStmt>(statements[0].node);
    require(select.order_by.size() == 2, "ORDER BY expression list not retained");
    const auto& add = asBinary(select.order_by[0].expression, BinaryOp::Add);
    require(asIdentifier(add.left).name.text == "score" &&
                select.order_by[0].direction == SortDirection::Desc,
            "ORDER BY arithmetic expression not retained");
    asAggregateExpr(select.order_by[1].expression, AggregateFunction::Count);
    require(select.order_by[1].direction == SortDirection::Asc,
            "ORDER BY aggregate expression direction not retained");
}

void testJoinTypes() {
    const auto statements = parseOk(
        "SELECT * FROM a INNER JOIN b ON a.id=b.id "
        "LEFT OUTER JOIN c ON a.id=c.id "
        "RIGHT JOIN d ON a.id=d.id "
        "FULL OUTER JOIN e ON a.id=e.id;");
    const auto& select = std::get<SelectStmt>(statements[0].node);
    require(select.joins.size() == 4, "join type list not retained");
    require(select.joins[0].type == JoinType::Inner &&
                select.joins[1].type == JoinType::Left &&
                select.joins[2].type == JoinType::Right &&
                select.joins[3].type == JoinType::Full,
            "join types not retained");
    expectSyntaxError("SELECT * FROM a LEFT b ON a.id=b.id;");
    expectSyntaxError("SELECT * FROM a OUTER JOIN b ON a.id=b.id;");
}

void testTableAndColumnAliases() {
    const auto statements = parseOk(
        "SELECT e.name AS employee_name,m.name manager_name "
        "FROM employee AS e JOIN employee m ON e.manager_id=m.id;"
        "UPDATE employee AS e SET e.name = 'Alice' WHERE e.id = 1;"
        "DELETE FROM employee e WHERE e.id = 2;");
    const auto& select = std::get<SelectStmt>(statements[0].node);
    require(select.table_alias && select.table_alias->text == "e",
            "explicit base-table alias lost");
    require(select.joins.size() == 1 && select.joins[0].alias &&
            select.joins[0].alias->text == "m", "implicit JOIN alias lost");
    require(select.column_aliases.size() == 2 &&
            select.column_aliases[0] &&
            select.column_aliases[0]->text == "employee_name" &&
            select.column_aliases[1] &&
            select.column_aliases[1]->text == "manager_name",
            "column aliases lost");

    const auto& update = std::get<UpdateStmt>(statements[1].node);
    require(update.table_alias && update.table_alias->text == "e",
            "UPDATE table alias lost");
    require(update.assignments[0].target.text == "e.name",
            "UPDATE qualified target with alias lost");
    require(asIdentifier(asBinary(update.where, BinaryOp::Equal).left).name.text == "e.id",
            "UPDATE alias-qualified WHERE lost");

    const auto& deletion = std::get<DeleteStmt>(statements[2].node);
    require(deletion.table_alias && deletion.table_alias->text == "e",
            "DELETE table alias lost");
    require(asIdentifier(asBinary(deletion.where, BinaryOp::Equal).left).name.text == "e.id",
            "DELETE alias-qualified WHERE lost");
}

void testBoolFloatAndNull() {
    const auto statements = parseOk(
        "CREATE TABLE metrics(id INT,active BOOL,score FLOAT);"
        "INSERT INTO metrics VALUES(1,TRUE,3.14);"
        "INSERT INTO metrics VALUES(2,FALSE,NULL);"
        "SELECT * FROM metrics WHERE active=TRUE AND score>=-0.5;");
    const auto& create = std::get<CreateTableStmt>(statements[0].node);
    require(create.columns[1].type == DataType::Bool && create.columns[2].type == DataType::Float,
            "extended column types lost");
    const auto& first = std::get<InsertStmt>(statements[1].node);
    require(std::get<bool>(first.values[1].value) &&
            std::get<double>(first.values[2].value) == 3.14, "extended literal value lost");
    const auto& second = std::get<InsertStmt>(statements[2].node);
    require(std::holds_alternative<NullValue>(second.values[2].value), "NULL literal lost");
    const auto& where = std::get<SelectStmt>(statements[3].node).where;
    require(asBinary(where, BinaryOp::And).right != nullptr, "extended predicate missing");
}

void testVarcharLength() {
    const auto statements =
        parseOk("CREATE TABLE student(id INT, name VARCHAR(20), note VARCHAR);");
    const auto& create = std::get<CreateTableStmt>(statements[0].node);
    require(create.columns[1].type == DataType::Varchar &&
            create.columns[1].varchar_length &&
            *create.columns[1].varchar_length == 20,
            "VARCHAR(n) length not retained");
    require(create.columns[2].type == DataType::Varchar &&
            !create.columns[2].varchar_length,
            "plain VARCHAR should not have a length");
    expectSyntaxError("CREATE TABLE t(name VARCHAR());");
    expectSyntaxError("CREATE TABLE t(name VARCHAR(0));");
    expectSyntaxError("CREATE TABLE t(name VARCHAR(-1));");
}

void testColumnConstraints() {
    const auto statements = parseOk(
        "CREATE TABLE account("
        "id INT PRIMARY KEY,"
        "name VARCHAR(20) NOT NULL UNIQUE DEFAULT 'guest',"
        "active BOOL DEFAULT TRUE,"
        "score FLOAT DEFAULT -1.5);");
    const auto& create = std::get<CreateTableStmt>(statements[0].node);
    require(create.columns[0].primary_key, "PRIMARY KEY constraint not retained");
    require(create.columns[1].not_null && create.columns[1].unique &&
                create.columns[1].default_value &&
                std::get<std::string>(create.columns[1].default_value->value) == "guest",
            "VARCHAR constraints not retained");
    require(create.columns[2].default_value &&
                std::get<bool>(create.columns[2].default_value->value),
            "BOOL DEFAULT not retained");
    require(create.columns[3].default_value &&
                std::get<double>(create.columns[3].default_value->value) == -1.5,
            "FLOAT DEFAULT not retained");
    expectSyntaxError("CREATE TABLE t(id INT PRIMARY);");
    expectSyntaxError("CREATE TABLE t(id INT NOT);");
    expectSyntaxError("CREATE TABLE t(id INT DEFAULT);");
    expectSyntaxError("CREATE TABLE t(id INT PRIMARY KEY PRIMARY KEY);");
    expectSyntaxError("CREATE TABLE t(id INT DEFAULT 1 DEFAULT 2);");
}

void testCreateTableIfNotExistsAndTableConstraints() {
    const auto statements = parseOk(
        "CREATE TABLE IF NOT EXISTS enrollment("
        "student_id INT,"
        "course_id INT,"
        "PRIMARY KEY(student_id, course_id),"
        "UNIQUE(course_id));");
    const auto& create = std::get<CreateTableStmt>(statements[0].node);
    require(create.if_not_exists, "CREATE TABLE IF NOT EXISTS flag not retained");
    require(create.columns.size() == 2, "CREATE TABLE columns lost around table constraints");
    require(create.table_constraints.size() == 2, "table constraints not retained");
    require(create.table_constraints[0].kind == TableConstraintKind::PrimaryKey &&
                create.table_constraints[0].columns.size() == 2 &&
                create.table_constraints[0].columns[1].text == "course_id",
            "table-level PRIMARY KEY columns not retained");
    require(create.table_constraints[1].kind == TableConstraintKind::Unique &&
                create.table_constraints[1].columns.size() == 1 &&
                create.table_constraints[1].columns[0].text == "course_id",
            "table-level UNIQUE columns not retained");

    expectSyntaxError("CREATE TABLE IF EXISTS t(id INT);");
    expectSyntaxError("CREATE TABLE IF NOT t(id INT);");
    expectSyntaxError("CREATE TABLE t(id INT, PRIMARY(id));");
    expectSyntaxError("CREATE TABLE t(id INT, UNIQUE());");
}

void testAlterTableAddColumn() {
    const auto statements = parseOk(
        "ALTER TABLE student ADD COLUMN email VARCHAR(50) NOT NULL DEFAULT 'x';"
        "ALTER TABLE student ADD score FLOAT;"
        "ALTER TABLE student DROP COLUMN email;"
        "ALTER TABLE student DROP score;"
        "ALTER TABLE student RENAME TO pupil;"
        "ALTER TABLE student RENAME COLUMN name TO full_name;");
    require(statements.size() == 6, "expected six ALTER TABLE statements");

    const auto& explicit_column = std::get<AlterTableStmt>(statements[0].node);
    require(explicit_column.table.text == "student", "ALTER TABLE target lost");
    const auto& add_email = std::get<AlterAddColumn>(explicit_column.action);
    require(add_email.column_keyword, "ALTER TABLE ADD COLUMN keyword marker lost");
    require(add_email.column.name.text == "email" &&
                add_email.column.type == DataType::Varchar &&
                add_email.column.varchar_length &&
                *add_email.column.varchar_length == 50 &&
                add_email.column.not_null &&
                add_email.column.default_value &&
                std::get<std::string>(add_email.column.default_value->value) == "x",
            "ALTER TABLE ADD COLUMN definition not retained");

    const auto& implicit_column = std::get<AlterTableStmt>(statements[1].node);
    const auto& add_score = std::get<AlterAddColumn>(implicit_column.action);
    require(!add_score.column_keyword &&
                add_score.column.name.text == "score" &&
                add_score.column.type == DataType::Float,
            "ALTER TABLE ADD without COLUMN not retained");

    const auto& explicit_drop = std::get<AlterTableStmt>(statements[2].node);
    const auto& drop_email = std::get<AlterDropColumn>(explicit_drop.action);
    require(drop_email.column_keyword && drop_email.column.text == "email",
            "ALTER TABLE DROP COLUMN not retained");

    const auto& implicit_drop = std::get<AlterTableStmt>(statements[3].node);
    const auto& drop_score = std::get<AlterDropColumn>(implicit_drop.action);
    require(!drop_score.column_keyword && drop_score.column.text == "score",
            "ALTER TABLE DROP without COLUMN not retained");

    const auto& rename_table = std::get<AlterTableStmt>(statements[4].node);
    const auto& rename_to = std::get<AlterRenameTable>(rename_table.action);
    require(rename_to.new_name.text == "pupil", "ALTER TABLE RENAME TO not retained");

    const auto& rename_column = std::get<AlterTableStmt>(statements[5].node);
    const auto& rename_name = std::get<AlterRenameColumn>(rename_column.action);
    require(rename_name.old_name.text == "name" && rename_name.new_name.text == "full_name",
            "ALTER TABLE RENAME COLUMN not retained");

    expectSyntaxError("ALTER student ADD COLUMN email INT;");
    expectSyntaxError("ALTER TABLE student COLUMN email INT;");
    expectSyntaxError("ALTER TABLE student ADD COLUMN;");
    expectSyntaxError("ALTER TABLE student DROP COLUMN;");
    expectSyntaxError("ALTER TABLE student RENAME pupil;");
    expectSyntaxError("ALTER TABLE student RENAME COLUMN name full_name;");
}

void testIsNullPredicates() {
    const auto statements = parseOk(
        "SELECT * FROM student WHERE score IS NULL;"
        "SELECT * FROM student WHERE name IS NOT NULL;");
    const auto& is_null = std::get<SelectStmt>(statements[0].node);
    const auto& null_check = asUnary(is_null.where, UnaryOp::IsNull);
    require(asIdentifier(null_check.operand).name.text == "score",
            "IS NULL operand not retained");
    const auto& is_not_null = std::get<SelectStmt>(statements[1].node);
    const auto& not_null_check = asUnary(is_not_null.where, UnaryOp::IsNotNull);
    require(asIdentifier(not_null_check.operand).name.text == "name",
            "IS NOT NULL operand not retained");
    expectSyntaxError("SELECT * FROM student WHERE score IS;");
    expectSyntaxError("SELECT * FROM student WHERE score IS NOT;");
    expectSyntaxError("SELECT * FROM student WHERE score IS TRUE;");
}

void testLimitOffset() {
    const auto statements = parseOk(
        "SELECT * FROM student ORDER BY id DESC LIMIT 10 OFFSET 20;"
        "SELECT name FROM student LIMIT 0;");
    const auto& first = std::get<SelectStmt>(statements[0].node);
    require(first.limit && *first.limit == 10, "LIMIT value not retained");
    require(first.offset && *first.offset == 20, "OFFSET value not retained");
    const auto& second = std::get<SelectStmt>(statements[1].node);
    require(second.limit && *second.limit == 0, "LIMIT 0 should be accepted");
    require(!second.offset, "absent OFFSET should remain empty");
    expectSyntaxError("SELECT * FROM student LIMIT;");
    expectSyntaxError("SELECT * FROM student LIMIT -1;");
    expectSyntaxError("SELECT * FROM student OFFSET 1;");
    expectSyntaxError("SELECT * FROM student LIMIT 1 OFFSET;");
}

void testLikePredicate() {
    const auto statements = parseOk(
        "SELECT * FROM student WHERE name LIKE 'A%';"
        "SELECT * FROM student WHERE name NOT LIKE 'B%';");
    const auto& select = std::get<SelectStmt>(statements[0].node);
    const auto& like = asBinary(select.where, BinaryOp::Like);
    require(asIdentifier(like.left).name.text == "name", "LIKE left operand not retained");
    const auto* pattern = std::get_if<LiteralExpr>(&like.right->node);
    require(pattern != nullptr && std::get<std::string>(pattern->value) == "A%",
            "LIKE pattern literal not retained");
    const auto& not_like_select = std::get<SelectStmt>(statements[1].node);
    const auto& not_like = asUnary(not_like_select.where, UnaryOp::Not);
    asBinary(not_like.operand, BinaryOp::Like);
    expectSyntaxError("SELECT * FROM student WHERE name LIKE;");
    expectSyntaxMessage("SELECT * FROM student WHERE name NOT 'A%';", "expected LIKE after NOT");
}

void testBetweenPredicates() {
    const auto statements = parseOk(
        "SELECT * FROM student WHERE age BETWEEN 18 AND 30;"
        "SELECT * FROM student WHERE age NOT BETWEEN 10 AND 20;");
    const auto& between_select = std::get<SelectStmt>(statements[0].node);
    const auto& between = asBinary(between_select.where, BinaryOp::And);
    const auto& lower = asBinary(between.left, BinaryOp::GreaterEqual);
    const auto& upper = asBinary(between.right, BinaryOp::LessEqual);
    require(asIdentifier(lower.left).name.text == "age" &&
                asIdentifier(upper.left).name.text == "age",
            "BETWEEN subject not retained in both comparisons");

    const auto& not_between_select = std::get<SelectStmt>(statements[1].node);
    const auto& not_between = asUnary(not_between_select.where, UnaryOp::Not);
    asBinary(not_between.operand, BinaryOp::And);
    expectSyntaxError("SELECT * FROM student WHERE age BETWEEN 18;");
    expectSyntaxMessage("SELECT * FROM student WHERE age NOT 18;", "expected LIKE after NOT, BETWEEN after NOT or IN after NOT");
}

void testInPredicates() {
    const auto statements = parseOk(
        "SELECT * FROM student WHERE id IN (1, 2, 3);"
        "SELECT * FROM student WHERE name NOT IN ('Alice', 'Bob');");
    const auto& in_select = std::get<SelectStmt>(statements[0].node);
    const auto& in_root = asBinary(in_select.where, BinaryOp::Or);
    asBinary(in_root.left, BinaryOp::Or);
    const auto& last_equal = asBinary(in_root.right, BinaryOp::Equal);
    require(asIdentifier(last_equal.left).name.text == "id", "IN subject not retained");

    const auto& not_in_select = std::get<SelectStmt>(statements[1].node);
    const auto& not_in = asUnary(not_in_select.where, UnaryOp::Not);
    asBinary(not_in.operand, BinaryOp::Or);
    expectSyntaxError("SELECT * FROM student WHERE id IN ();");
    expectSyntaxError("SELECT * FROM student WHERE id IN (1,);");
    expectSyntaxError("SELECT * FROM student WHERE id IN (age);");
}

void testInSubqueries() {
    const auto statements = parseOk(
        "SELECT name FROM student WHERE id IN "
        "(SELECT student_id FROM score WHERE value > 60);"
        "SELECT name FROM student WHERE id NOT IN "
        "(SELECT student_id FROM score);");
    const auto& in_select = std::get<SelectStmt>(statements[0].node);
    const auto& in_subquery = asInSubquery(in_select.where, false);
    require(asIdentifier(in_subquery.value).name.text == "id",
            "IN subquery left value not retained");
    require(in_subquery.query->table.text == "score",
            "IN subquery table not retained");
    require(std::get<std::vector<Identifier>>(in_subquery.query->columns)[0].text == "student_id",
            "IN subquery select list not retained");
    asBinary(in_subquery.query->where, BinaryOp::Greater);

    const auto& not_in_select = std::get<SelectStmt>(statements[1].node);
    const auto& not_in_subquery = asInSubquery(not_in_select.where, true);
    require(not_in_subquery.query->table.text == "score",
            "NOT IN subquery table not retained");

    expectSyntaxError("SELECT * FROM student WHERE id IN (SELECT student_id FROM score;");
    expectSyntaxError("SELECT * FROM student WHERE id IN (SELECT FROM score);");
}

void testExistsSubqueries() {
    const auto statements = parseOk(
        "SELECT name FROM student WHERE EXISTS "
        "(SELECT * FROM score WHERE score.student_id = student.id);"
        "SELECT name FROM student WHERE NOT EXISTS "
        "(SELECT student_id FROM score);");
    const auto& exists_select = std::get<SelectStmt>(statements[0].node);
    const auto& exists = asExistsSubquery(exists_select.where, false);
    require(exists.query->table.text == "score",
            "EXISTS subquery table not retained");
    require(std::holds_alternative<AllColumns>(exists.query->columns),
            "EXISTS subquery SELECT * not retained");
    asBinary(exists.query->where, BinaryOp::Equal);

    const auto& not_exists_select = std::get<SelectStmt>(statements[1].node);
    const auto& not_exists = asExistsSubquery(not_exists_select.where, true);
    require(not_exists.query->table.text == "score",
            "NOT EXISTS subquery table not retained");

    expectSyntaxError("SELECT * FROM student WHERE EXISTS SELECT * FROM score;");
    expectSyntaxError("SELECT * FROM student WHERE EXISTS (SELECT * FROM score;");
    expectSyntaxError("SELECT * FROM student WHERE NOT EXISTS;");
}

void testDerivedTables() {
    const auto statements = parseOk(
        "SELECT d.name FROM (SELECT name, age FROM student WHERE age > 18) AS d "
        "WHERE d.age > 20;"
        "SELECT s.name FROM student s JOIN "
        "(SELECT student_id FROM score WHERE value > 60) x "
        "ON s.id = x.student_id;");

    const auto& from_derived_select = std::get<SelectStmt>(statements[0].node);
    const auto& from_source = asDerivedTable(from_derived_select.from, "d");
    require(from_derived_select.table.text.empty(),
            "derived table should not masquerade as a physical table");
    require(from_source.subquery->table.text == "student",
            "derived table subquery source not retained");
    require(std::get<std::vector<Identifier>>(from_source.subquery->columns).size() == 2,
            "derived table subquery SELECT list not retained");
    asBinary(from_source.subquery->where, BinaryOp::Greater);
    asBinary(from_derived_select.where, BinaryOp::Greater);

    const auto& join_derived_select = std::get<SelectStmt>(statements[1].node);
    require(join_derived_select.table.text == "student" && join_derived_select.table_alias &&
                join_derived_select.table_alias->text == "s",
            "base table alias changed while parsing derived JOIN");
    require(join_derived_select.joins.size() == 1, "derived JOIN not retained");
    const auto& join_source = asDerivedTable(join_derived_select.joins[0].source, "x");
    require(join_derived_select.joins[0].table.text.empty(),
            "derived JOIN should not masquerade as a physical table");
    require(join_source.subquery->table.text == "score",
            "derived JOIN subquery source not retained");
    asBinary(join_derived_select.joins[0].on, BinaryOp::Equal);

    expectSyntaxError("SELECT * FROM (SELECT id FROM student);");
    expectSyntaxError("SELECT * FROM (SELECT id FROM student;");
    expectSyntaxError("SELECT * FROM (UPDATE student SET age = 1) d;");
}

void testScalarSubqueries() {
    const auto statements = parseOk(
        "SELECT name FROM student WHERE age > "
        "(SELECT age FROM score WHERE score.student_id = student.id);"
        "SELECT (SELECT COUNT(*) FROM score) AS score_count FROM student;");

    const auto& where_select = std::get<SelectStmt>(statements[0].node);
    const auto& comparison = asBinary(where_select.where, BinaryOp::Greater);
    const auto& scalar = asScalarSubquery(comparison.right);
    require(scalar.query->table.text == "score",
            "scalar subquery table not retained");
    require(std::get<std::vector<Identifier>>(scalar.query->columns)[0].text == "age",
            "scalar subquery SELECT list not retained");
    asBinary(scalar.query->where, BinaryOp::Equal);

    const auto& item_select = std::get<SelectStmt>(statements[1].node);
    const auto& items = std::get<std::vector<SelectItem>>(item_select.columns);
    require(items.size() == 1, "scalar subquery SELECT item count changed");
    asScalarSubquery(std::get<ExprPtr>(items[0]));
    require(item_select.column_aliases.size() == 1 && item_select.column_aliases[0] &&
                item_select.column_aliases[0]->text == "score_count",
            "scalar subquery output alias not retained");

    expectSyntaxError("SELECT name FROM student WHERE age > (SELECT age FROM score;");
    expectSyntaxError("SELECT name FROM student WHERE age > (SELECT FROM score);");
}

void testSetOperations() {
    const auto statements = parseOk(
        "SELECT id FROM student UNION SELECT student_id FROM score;"
        "SELECT id FROM student UNION ALL SELECT student_id FROM score "
        "UNION SELECT id FROM archive;"
        "SELECT id FROM student INTERSECT SELECT student_id FROM score;"
        "SELECT id FROM student EXCEPT ALL SELECT student_id FROM score;");

    const auto& distinct_union = std::get<SelectStmt>(statements[0].node);
    require(distinct_union.table.text == "student", "UNION left SELECT source not retained");
    require(distinct_union.set_operations.size() == 1, "UNION operation not retained");
    require(distinct_union.set_operations[0].op == SetOperator::Union,
            "UNION operator kind not retained");
    require(!distinct_union.set_operations[0].all, "UNION should default to distinct");
    require(distinct_union.set_operations[0].query->table.text == "score",
            "UNION right SELECT source not retained");
    require(std::get<std::vector<Identifier>>(
                distinct_union.set_operations[0].query->columns)[0].text == "student_id",
            "UNION right SELECT list not retained");

    const auto& chained_union = std::get<SelectStmt>(statements[1].node);
    require(chained_union.set_operations.size() == 2, "UNION chain not retained");
    require(chained_union.set_operations[0].all, "UNION ALL flag not retained");
    require(!chained_union.set_operations[1].all, "second UNION should default to distinct");
    require(chained_union.set_operations[0].query->table.text == "score" &&
                chained_union.set_operations[1].query->table.text == "archive",
            "UNION chain right SELECT sources not retained");

    const auto& intersect = std::get<SelectStmt>(statements[2].node);
    require(intersect.set_operations.size() == 1 &&
                intersect.set_operations[0].op == SetOperator::Intersect &&
                !intersect.set_operations[0].all,
            "INTERSECT operation not retained");

    const auto& except_all = std::get<SelectStmt>(statements[3].node);
    require(except_all.set_operations.size() == 1 &&
                except_all.set_operations[0].op == SetOperator::Except &&
                except_all.set_operations[0].all,
            "EXCEPT ALL operation not retained");

    expectSyntaxError("SELECT id FROM student UNION;");
    expectSyntaxError("SELECT id FROM student UNION ALL;");
    expectSyntaxError("SELECT id FROM student UNION UPDATE student SET id = 1;");
    expectSyntaxError("SELECT id FROM student INTERSECT;");
    expectSyntaxError("SELECT id FROM student EXCEPT ALL;");
}

void testCaseExpressions() {
    const auto statements = parseOk(
        "SELECT CASE WHEN age >= 18 THEN 'adult' ELSE 'minor' END AS label "
        "FROM student WHERE CASE WHEN active THEN TRUE ELSE FALSE END;"
        "SELECT CASE active WHEN TRUE THEN 'yes' WHEN FALSE THEN 'no' ELSE 'unknown' END "
        "FROM student;");

    const auto& searched_select = std::get<SelectStmt>(statements[0].node);
    const auto& items = std::get<std::vector<SelectItem>>(searched_select.columns);
    const auto& searched = asCaseExpr(std::get<ExprPtr>(items[0]));
    require(searched.operand == nullptr, "searched CASE should not have operand");
    require(searched.branches.size() == 1 && searched.else_result,
            "searched CASE branches or ELSE not retained");
    asBinary(searched.branches[0].condition, BinaryOp::GreaterEqual);
    require(std::get<std::string>(
                std::get<LiteralExpr>(searched.branches[0].result->node).value) == "adult",
            "searched CASE THEN result not retained");
    require(searched_select.column_aliases.size() == 1 && searched_select.column_aliases[0] &&
                searched_select.column_aliases[0]->text == "label",
            "CASE select item alias not retained");
    const auto& where_case = asCaseExpr(searched_select.where);
    require(where_case.branches.size() == 1 && where_case.else_result,
            "CASE in WHERE not retained");

    const auto& simple_select = std::get<SelectStmt>(statements[1].node);
    const auto& simple_items = std::get<std::vector<SelectItem>>(simple_select.columns);
    const auto& simple = asCaseExpr(std::get<ExprPtr>(simple_items[0]));
    require(simple.operand != nullptr, "simple CASE operand not retained");
    asIdentifier(simple.operand);
    require(simple.branches.size() == 2 && simple.else_result,
            "simple CASE WHEN branches not retained");

    expectSyntaxError("SELECT CASE age THEN 1 END FROM student;");
    expectSyntaxError("SELECT CASE WHEN age > 18 'adult' END FROM student;");
    expectSyntaxError("SELECT CASE WHEN age > 18 THEN 'adult' FROM student;");
}

void testAggregateSelectItems() {
    const auto statements = parseOk(
        "SELECT active, COUNT(*), COUNT(id), AVG(score) AS avg_score, "
        "MIN(score), MAX(score), SUM(score) FROM metrics GROUP BY active;");
    const auto& select = std::get<SelectStmt>(statements[0].node);
    const auto& items = std::get<std::vector<SelectItem>>(select.columns);
    require(items.size() == 7, "aggregate select items not retained");
    const auto* first = std::get_if<Identifier>(&items[0]);
    require(first != nullptr && first->text == "active", "plain select item lost");
    require(std::holds_alternative<AllColumns>(
                asAggregate(items[1], AggregateFunction::Count).argument),
            "COUNT(*) argument not retained");
    const auto& count_id = asAggregate(items[2], AggregateFunction::Count);
    require(std::get<Identifier>(count_id.argument).text == "id", "COUNT(column) argument lost");
    const auto& avg = asAggregate(items[3], AggregateFunction::Avg);
    require(std::get<Identifier>(avg.argument).text == "score", "AVG argument lost");
    require(select.column_aliases.size() == 7 && select.column_aliases[3] &&
                select.column_aliases[3]->text == "avg_score",
            "aggregate alias lost");
    asAggregate(items[4], AggregateFunction::Min);
    asAggregate(items[5], AggregateFunction::Max);
    asAggregate(items[6], AggregateFunction::Sum);
    expectSyntaxError("SELECT COUNT FROM metrics;");
    expectSyntaxError("SELECT COUNT() FROM metrics;");
    expectSyntaxError("SELECT SUM(*) FROM metrics;");
    expectSyntaxError("SELECT AVG(score FROM metrics;");
}

void testAggregateExpressions() {
    const auto statements = parseOk(
        "SELECT COUNT(*) + 1 AS count_plus_one FROM metrics;"
        "SELECT active FROM metrics GROUP BY active HAVING COUNT(*) > 0;");
    const auto& select_expr = std::get<SelectStmt>(statements[0].node);
    const auto& items = std::get<std::vector<SelectItem>>(select_expr.columns);
    require(items.size() == 1, "aggregate expression item not retained");
    const auto& add = asBinary(std::get<ExprPtr>(items[0]), BinaryOp::Add);
    require(std::holds_alternative<AllColumns>(
                asAggregateExpr(add.left, AggregateFunction::Count).argument),
            "COUNT(*) expression argument not retained");
    require(select_expr.column_aliases.size() == 1 && select_expr.column_aliases[0] &&
                select_expr.column_aliases[0]->text == "count_plus_one",
            "aggregate expression alias not retained");

    const auto& having_select = std::get<SelectStmt>(statements[1].node);
    const auto& greater = asBinary(having_select.having, BinaryOp::Greater);
    asAggregateExpr(greater.left, AggregateFunction::Count);
}

void testHavingClause() {
    const auto statements = parseOk(
        "SELECT active, COUNT(*) FROM metrics GROUP BY active "
        "HAVING active = TRUE ORDER BY active DESC LIMIT 5;");
    const auto& select = std::get<SelectStmt>(statements[0].node);
    require(select.having != nullptr, "HAVING expression not retained");
    asBinary(select.having, BinaryOp::Equal);
    require(select.order_by.size() == 1 && select.limit && *select.limit == 5,
            "clauses after HAVING not retained");
    expectSyntaxError("SELECT active FROM metrics GROUP BY active HAVING;");
    expectSyntaxError("SELECT active FROM metrics ORDER BY active HAVING active = TRUE;");
}

void testIntegerBoundaries() {
    const auto statements =
        parseOk("SELECT * FROM t WHERE -9223372036854775808 < id;");
    const auto& select = std::get<SelectStmt>(statements[0].node);
    const BinaryExpr& less = asBinary(select.where, BinaryOp::Less);
    const auto* literal = std::get_if<LiteralExpr>(&less.left->node);
    require(literal != nullptr, "expected negative integer literal");
    require(std::get<std::int64_t>(literal->value) == std::numeric_limits<std::int64_t>::min(),
            "expected INT64_MIN");

    expectSyntaxError("SELECT * FROM t WHERE 9223372036854775808 > id;",
                      ErrorCode::IntegerOutOfRange);
    expectSyntaxError("SELECT * FROM t WHERE -9223372036854775809 < id;",
                      ErrorCode::IntegerOutOfRange);
}

void testSyntaxErrors() {
    expectSyntaxError("SELECT * FROM t");
    expectSyntaxError("SELECT * FROM t WHERE (id = 1;");
    expectSyntaxError("SELECT id t;");
    expectSyntaxError("INSERT INTO t(id) (1);");
    expectSyntaxError("SELECT * FROM t WHERE a < b < c;");
    expectSyntaxError("SELECT * FROM t JOIN u;");
    expectSyntaxError("SELECT t. FROM t;");
    expectSyntaxError("SELECT * FROM t GROUP age;");
    expectSyntaxError("SELECT * FROM t ORDER BY age GROUP BY id;");
}

void testSyntaxErrorMessages() {
    expectSyntaxMessage("BOGUS;", "expected CREATE, ALTER, DROP, INSERT, SELECT, UPDATE or DELETE");
    expectSyntaxMessage("BOGUS;", "identifier \"BOGUS\"");
    expectSyntaxMessage("SELECT id, * FROM t;", "expected column name, aggregate function or expression");
    expectSyntaxMessage("SELECT * FROM t ORDER BY age GROUP BY id;", "JOIN -> WHERE -> GROUP BY -> HAVING -> ORDER BY -> LIMIT/OFFSET");
    expectSyntaxMessage("INSERT INTO t VALUES (WHERE);", "expected integer, float, string, TRUE, FALSE or NULL");
}

void testLogicalOperatorLocations() {
    const std::string sql = "SELECT * FROM t WHERE a = 1 OR b = 2 AND NOT c = 3;";
    const auto statements = parseOk(sql);
    const auto& root = asBinary(std::get<SelectStmt>(statements[0].node).where, BinaryOp::Or);
    const auto& rhs = asBinary(root.right, BinaryOp::And);
    require(root.operator_span->begin.offset == sql.find("OR") && root.operator_span->end.offset == sql.find("OR") + 2,
            "OR location must not depend on argument evaluation order");
    require(rhs.operator_span->begin.offset == sql.find("AND") && rhs.operator_span->end.offset == sql.find("AND") + 3,
            "AND location must refer to operator, not RHS last token");
}

void testEofContract() {
    const SourceSpan at{{0, 1, 1}, {0, 1, 1}};
    for (const TokenStream& tokens : {TokenStream{}, TokenStream{{TokenKind::Minus, "-", at}},
            TokenStream{{TokenKind::EndOfInput, "", at}, {TokenKind::EndOfInput, "", at}},
            TokenStream{{TokenKind::EndOfInput, "", at}, {TokenKind::Select, "SELECT", at}}}) {
        const auto result = parse(tokens);
        require(std::holds_alternative<Diagnostic>(result), "invalid EOF protocol must be rejected");
        require(std::get<Diagnostic>(result).code == ErrorCode::UnexpectedToken, "unexpected EOF diagnostic");
    }
}

void testDepthLimits() {
    parseOk("SELECT * FROM t WHERE " + std::string(256, '(') + "1=1" + std::string(256, ')') + ";");
    expectSyntaxError("SELECT * FROM t WHERE " + std::string(257, '(') + "1=1" + std::string(257, ')') + ";", ErrorCode::ExpressionTooDeep);
    std::string chain = "SELECT * FROM t WHERE 1";
    for (int i = 0; i < 255; ++i) chain += "+1";
    parseOk(chain + ";"); // 左结合 AST 恰好 256 层。
    expectSyntaxError(chain + "+1;", ErrorCode::ExpressionTooDeep);
    std::string nots;
    for (int i = 0; i < 300; ++i) nots += "NOT ";
    expectSyntaxError("SELECT * FROM t WHERE " + nots + "id=1;", ErrorCode::ExpressionTooDeep);
    parseOk("SELECT * FROM t WHERE id=1;"); // 失败不泄露计数到下次调用。
}

void testAssociativityAndOwnedAst() {
    const auto statements = parseOk(std::string("UPDATE t SET age = age - 2 - 3 * 4;"));
    const auto& update = std::get<UpdateStmt>(statements[0].node);
    const auto& subtract = asBinary(update.assignments[0].value, BinaryOp::Subtract);
    asBinary(subtract.left, BinaryOp::Subtract);
    asBinary(subtract.right, BinaryOp::Multiply);
    require(update.table.text == "t", "AST must own text after token stream is destroyed");
}

} // namespace

int main() {
    try {
        testStatements();
        testMultiRowInsert();
        testDropTable();
        testSelectStarAndEmptyInput();
        testDistinctSelect();
        testSelectExpressionItems();
        testExpressionPrecedence();
        testJoinGroupOrderAndQualifiedNames();
        testOrderByExpressions();
        testJoinTypes();
        testTableAndColumnAliases();
        testBoolFloatAndNull();
        testVarcharLength();
        testColumnConstraints();
        testCreateTableIfNotExistsAndTableConstraints();
        testAlterTableAddColumn();
        testIsNullPredicates();
        testLimitOffset();
        testLikePredicate();
        testBetweenPredicates();
        testInPredicates();
        testInSubqueries();
        testExistsSubqueries();
        testDerivedTables();
        testScalarSubqueries();
        testSetOperations();
        testCaseExpressions();
        testAggregateSelectItems();
        testAggregateExpressions();
        testHavingClause();
        testIntegerBoundaries();
        testSyntaxErrors();
        testSyntaxErrorMessages();
        testLogicalOperatorLocations();
        testEofContract();
        testDepthLimits();
        testAssociativityAndOwnedAst();
        std::cout << "Parser tests passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
