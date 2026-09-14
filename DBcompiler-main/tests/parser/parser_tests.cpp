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
    expectSyntaxMessage("BOGUS;", "expected CREATE, DROP, INSERT, SELECT, UPDATE or DELETE");
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

void testExplainAnalyze() {
    const auto statements = parseOk(
        "EXPLAIN SELECT id FROM student WHERE id>1;"
        "EXPLAIN ANALYZE UPDATE student SET id=id+1 WHERE id=1;");
    require(statements.size() == 2, "expected two EXPLAIN statements");
    const auto& plain = std::get<ExplainStmt>(statements[0].node);
    require(!plain.analyze && std::holds_alternative<SelectStmt>(plain.target),
            "plain EXPLAIN target was not retained");
    const auto& analyzed = std::get<ExplainStmt>(statements[1].node);
    require(analyzed.analyze && std::holds_alternative<UpdateStmt>(analyzed.target),
            "EXPLAIN ANALYZE target was not retained");
    expectSyntaxError("EXPLAIN EXPLAIN SELECT * FROM student;");
    expectSyntaxError("EXPLAIN ANALYZE;");
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
        testIsNullPredicates();
        testLimitOffset();
        testLikePredicate();
        testBetweenPredicates();
        testInPredicates();
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
        testExplainAnalyze();
        std::cout << "Parser tests passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
