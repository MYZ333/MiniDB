// A/B 兼容性回归：每个输入从真实 SQL 进入 Lexer/Parser，再进入已有语义/计划模块。
// 测试驱动显式注册 CREATE 的模式，不执行数据记录读写。
#include "minisql/lexer.hpp"
#include "minisql/parser.hpp"
#include "minisql/memory_catalog.hpp"
#include "minisql/plan_printer.hpp"
#include "../test_support.hpp"

#include <limits>

using namespace minisql;
using namespace minisql::test;

namespace {
std::vector<Statement> statements(const std::string& sql) {
    auto tokens = value(lex(sql));
    return value(parse(tokens)); // 返回的 AST 不再依赖此处的临时 SQL/Token 生命周期。
}

struct Fixture {
    MemoryCatalog catalog;
    Fixture() { value(catalog.createTable("student", {{"id", DataType::Int}, {"name", DataType::Varchar}, {"age", DataType::Int}})); }
    Result<BoundStatement> bind(const std::string& sql) {
        const auto parsed = statements(sql);
        check(parsed.size() == 1, "fixture expects one statement");
        return analyze(parsed[0], *catalog.snapshot());
    }
    LogicalPlan compile(const std::string& sql) { return value(buildPlan(value(bind(sql)))); }
};

void at(const Diagnostic& error, const std::string& sql, const std::string& lexeme) {
    const auto offset = sql.find(lexeme);
    check(offset != std::string::npos && error.span.has_value(), "test needs a real SQL location");
    check(error.span->begin.offset == offset && error.span->end.offset == offset + lexeme.size(), "diagnostic range lost across module boundary");
}
} // namespace

int main() {
    Suite suite;
    suite.run("SQL five-statement pipeline with explicit catalog registration", [] {
        const auto parsed = statements(
            "CREATE TABLE Student(id INT,name VARCHAR,age INT);"
            "INSERT INTO student(name,age,id) VALUES ('Alice',20,1);"
            "SELECT name FROM student WHERE age>18;"
            "UPDATE student SET age=age+1 WHERE id=1;"
            "DELETE FROM student WHERE id=1;/*closed at EOF*/");
        check(parsed.size() == 5, "five statements expected");
        MemoryCatalog catalog;
        std::vector<LogicalPlan> plans;
        for (const auto& statement : parsed) {
            const auto bound = value(analyze(statement, *catalog.snapshot()));
            plans.push_back(value(buildPlan(bound)));
            if (const auto* create = std::get_if<BoundCreateTable>(&bound.node)) {
                check(catalog.snapshot()->version() == 0, "compiler mutated catalog");
                value(catalog.createTable(create->table_name, create->columns));
            }
        }
        check(std::holds_alternative<CreateTablePlan>(plans[0].root->node), "CREATE root mismatch");
        const auto& row = std::get<InsertPlan>(plans[1].root->node).values;
        check(std::get<std::int64_t>(row[0]) == 1 && std::get<std::string>(row[1]) == "Alice", "INSERT order incompatible");
        check(std::holds_alternative<ProjectPlan>(plans[2].root->node), "SELECT root mismatch");
        const auto& update = std::get<UpdatePlan>(plans[3].root->node);
        const auto& deletion = std::get<DeletePlan>(plans[4].root->node);
        check(update.input->carries_row_id && deletion.input->carries_row_id, "DML row identity missing");
        check(plans[0].catalog_version == 0 && plans[4].catalog_version == 1 && catalog.snapshot()->version() == 1,
              "compilation changed metadata version unexpectedly");
    });
    suite.run("SQL boolean precedence survives binding and planning", [] {
        Fixture f;
        const auto bound = value(f.bind("SELECT * FROM student WHERE id=1 OR age=2 AND NOT id=3;"));
        const auto& where = std::get<BoundSelect>(bound.node).where;
        const auto& root = std::get<BoundBinary>(where->node);
        check(root.op == BinaryOp::Or, "OR precedence wrong");
        const auto& rhs = std::get<BoundBinary>(root.right->node);
        check(rhs.op == BinaryOp::And && std::get<BoundUnary>(rhs.right->node).op == UnaryOp::Not,
              "AND/NOT precedence wrong");
        check(std::get<BoundBinary>(std::get<BoundUnary>(rhs.right->node).operand->node).op == BinaryOp::Equal,
              "NOT must wrap comparison");
        check(formatPlan(value(buildPlan(bound))).find("OR") != std::string::npos, "boolean plan formatting failed");
    });
    suite.run("SQL arithmetic and parentheses retain AST meaning", [] {
        Fixture f;
        auto plan = f.compile("UPDATE student SET age=(age-2)-3*4 WHERE NOT (id=1 OR age<0);");
        const auto& expr = std::get<UpdatePlan>(plan.root->node).assignments[0].value;
        const auto& subtract = std::get<BoundBinary>(expr->node);
        check(subtract.op == BinaryOp::Subtract && std::get<BoundBinary>(subtract.left->node).op == BinaryOp::Subtract &&
              std::get<BoundBinary>(subtract.right->node).op == BinaryOp::Multiply, "arithmetic precedence changed");
    });
    suite.run("SQL string decoding signed limits and ownership", [] {
        Fixture f;
        auto plan = f.compile(std::string("INSERT INTO student VALUES (-9223372036854775808,'Tom''s 书',9223372036854775807);"));
        const auto& values = std::get<InsertPlan>(plan.root->node).values;
        check(std::get<std::int64_t>(values[0]) == std::numeric_limits<std::int64_t>::min() &&
              std::get<std::int64_t>(values[2]) == std::numeric_limits<std::int64_t>::max(), "signed integer boundary lost");
        check(std::get<std::string>(values[1]) == "Tom's 书", "string decoding/ownership mismatch");
    });
    suite.run("SQL UPDATE swap and DELETE all rows", [] {
        Fixture f;
        auto update = f.compile("UPDATE student SET id=age,age=id;");
        const auto& assignments = std::get<UpdatePlan>(update.root->node).assignments;
        check(std::get<BoundColumnRef>(assignments[0].value->node).ordinal == 2 &&
              std::get<BoundColumnRef>(assignments[1].value->node).ordinal == 0, "old-row references lost");
        const auto deletion = f.compile("DELETE FROM student;");
        const auto input = std::get<DeletePlan>(deletion.root->node).input;
        check(std::holds_alternative<SeqScanPlan>(input->node) && input->carries_row_id, "DELETE without WHERE generated wrong source");
    });
    suite.run("SQL semantic name and value errors retain real positions", [] {
        Fixture f;
        const std::string missing = "SELECT score FROM student;";
        at(failure(f.bind(missing), ErrorCode::ColumnNotFound), missing, "score");
        const std::string wrong = "INSERT INTO student(name,age,id) VALUES ('Alice',20,'bad');";
        at(failure(f.bind(wrong), ErrorCode::TypeMismatch), wrong, "'bad'");
        const std::string duplicate = "UPDATE student SET age=1,AGE=2;";
        at(failure(f.bind(duplicate), ErrorCode::DuplicateAssignment), duplicate, "AGE");
    });
    suite.run("SQL logical operator diagnostics locate AND and OR", [] {
        Fixture f;
        for (const std::string op : {"AND", "OR"}) {
            const auto sql = "SELECT * FROM student WHERE id=1 " + op + " age;";
            at(failure(f.bind(sql), ErrorCode::InvalidOperandType), sql, op);
        }
    });
    suite.run("CRLF comments preserve byte offsets and semantic line columns", [] {
        Fixture f;
        const std::string sql = "-- comment\r\nSELECT name FROM student\r\nWHERE age + 'x' > 1;/*tail*/";
        auto error = failure(f.bind(sql), ErrorCode::InvalidOperandType);
        at(error, sql, "+");
        check(error.span->begin.line == 3 && error.span->begin.column == 11, "CRLF line/column incompatible");
    });
    suite.run("lexical and syntax failures are distinguished before B", [] {
        failure(lex("@"), ErrorCode::InvalidCharacter, DiagnosticStage::Lexical);
        failure(lex("/*missing"), ErrorCode::UnterminatedComment, DiagnosticStage::Lexical);
        failure(lex("'abc"), ErrorCode::UnterminatedString, DiagnosticStage::Lexical);
        for (const std::string sql : {"SELECT * FROM student", "SELECT * FROM student WHERE id=1 AND;", "SELECT * FROM student WHERE id<age<3;"}) {
            auto tokens = value(lex(sql));
            failure(parse(tokens), ErrorCode::UnexpectedToken, DiagnosticStage::Syntax);
        }
        const auto tokens = value(lex("INSERT INTO t VALUES (9223372036854775808);"));
        failure(parse(tokens), ErrorCode::IntegerOutOfRange, DiagnosticStage::Syntax);
    });
    suite.run("CREATE compilation requires explicit metadata transition", [] {
        MemoryCatalog catalog;
        const auto parsed = statements("CREATE TABLE t(id INT); SELECT * FROM t;");
        const auto old = catalog.snapshot();
        auto created = value(analyze(parsed[0], *old));
        value(buildPlan(created));
        failure(analyze(parsed[1], *old), ErrorCode::TableNotFound);
        const auto& create = std::get<BoundCreateTable>(created.node);
        value(catalog.createTable(create.table_name, create.columns));
        value(buildPlan(value(analyze(parsed[1], *catalog.snapshot()))));
        failure(analyze(parsed[1], *old), ErrorCode::TableNotFound);
    });
    suite.run("case-insensitive names and repeated SELECT columns through SQL", [] {
        Fixture f;
        const auto plan = f.compile("sElEcT NAME,id,Name FROM STUDENT WHERE AGE>18;");
        check(plan.root->output.size() == 3 && plan.root->output[0].name == "name" &&
              plan.root->output[1].name == "id" && plan.root->output[2].name == "name", "name contract mismatch");
    });
    suite.run("excessive syntax depth stops before semantic analysis", [] {
        const auto tokens = value(lex("SELECT * FROM t WHERE " + std::string(300, '(') + "1=1" + std::string(300, ')') + ";"));
        failure(parse(tokens), ErrorCode::ExpressionTooDeep, DiagnosticStage::Syntax);
        Fixture f;
        value(f.bind("SELECT * FROM student WHERE id=1;"));
    });
    return suite.finish();
}
