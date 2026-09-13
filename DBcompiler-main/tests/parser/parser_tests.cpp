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

void testSelectStarAndEmptyInput() {
    const auto empty = parseOk("   -- only comment\n");
    require(empty.empty(), "empty input should parse as zero statements");

    const auto statements = parseOk("SELECT * FROM student;");
    require(statements.size() == 1, "expected one SELECT statement");
    const auto& select = std::get<SelectStmt>(statements[0].node);
    require(std::holds_alternative<AllColumns>(select.columns), "expected SELECT *");
    require(select.where == nullptr, "SELECT without WHERE should have null where");
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

void testTableAndColumnAliases() {
    const auto statements = parseOk(
        "SELECT e.name AS employee_name,m.name manager_name "
        "FROM employee AS e JOIN employee m ON e.manager_id=m.id;");
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
        testSelectStarAndEmptyInput();
        testExpressionPrecedence();
        testJoinGroupOrderAndQualifiedNames();
        testTableAndColumnAliases();
        testBoolFloatAndNull();
        testIntegerBoundaries();
        testSyntaxErrors();
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
