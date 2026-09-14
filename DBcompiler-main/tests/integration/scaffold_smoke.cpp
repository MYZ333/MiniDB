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
    suite.run("new A aggregate AST still reaches B aggregate plans", [] {
        Fixture f;
        const auto plan = f.compile(
            "SELECT (age) AS years, (COUNT(*)) AS rows, SUM(id) AS total "
            "FROM student GROUP BY age ORDER BY total DESC;");
        const auto& aggregate = std::get<AggregatePlan>(plan.root->node);
        check(aggregate.items.size() == 3 && plan.root->output[1].name == "rows" &&
              std::get<std::size_t>(aggregate.order_by[0].key) == 2,
              "A expression wrappers or parallel aliases broke B aggregation");
    });
    suite.run("A syntax extensions compile through B instead of being ignored", [] {
        Fixture f;
        for (const std::string sql : {
            "SELECT DISTINCT name FROM student;",
            "SELECT * FROM student LIMIT 0;",
            "SELECT * FROM student LIMIT 1 OFFSET 0;",
            "SELECT age, COUNT(*) FROM student GROUP BY age HAVING COUNT(*) > 1;",
            "SELECT * FROM student s LEFT JOIN student t ON s.id=t.id;",
            "SELECT * FROM student s RIGHT JOIN student t ON s.id=t.id;",
            "SELECT * FROM student s FULL JOIN student t ON s.id=t.id;",
            "SELECT age+1 FROM student;",
            "SELECT COUNT(*)+1 FROM student;",
            "SELECT name FROM student ORDER BY age+1;",
            "SELECT COUNT(*) FROM student ORDER BY COUNT(*);",
            "SELECT * FROM student WHERE name LIKE 'A%';",
            "SELECT * FROM student WHERE name NOT LIKE 'A%';",
            "INSERT INTO student VALUES(1,'a',2),(2,'b',3);",
            "DROP TABLE IF EXISTS student;",
            "CREATE TABLE constrained(id INT PRIMARY KEY);",
            "CREATE TABLE constrained(id INT NOT NULL);",
            "CREATE TABLE constrained(id INT UNIQUE);",
            "CREATE TABLE constrained(id INT DEFAULT 1);",
            "CREATE TABLE constrained(name VARCHAR(20));"
        }) {
            f.compile(sql);
        }
        check(f.catalog.snapshot()->version() == 1 &&
              f.catalog.snapshot()->findTable("student"), "compilation must not mutate the catalog");
    });
    suite.run("new scalar syntax and DML aliases bind through existing contracts", [] {
        Fixture f;
        for (const std::string sql : {
            "SELECT * FROM student WHERE id <> 1;",
            "SELECT * FROM student WHERE age BETWEEN 18 AND 30;",
            "SELECT * FROM student WHERE id NOT IN (1,2);",
            "SELECT * FROM student WHERE name IS NULL;",
            "SELECT * FROM student WHERE age IS NOT NULL;",
            "SELECT * FROM student WHERE NULL IS NULL;",
            "UPDATE student s SET s.age=s.age+1 WHERE s.id=1;",
            "DELETE FROM student AS s WHERE s.name IS NULL;",
            "SELECT s.id FROM student s INNER JOIN student t ON s.id=t.id;"
        }) f.compile(sql);
        failure(f.bind("UPDATE student s SET student.age=1;"), ErrorCode::ColumnNotFound);
        failure(f.bind("DELETE FROM student s WHERE student.id=1;"), ErrorCode::ColumnNotFound);
    });
    suite.run("DDL constraints supply defaults and reject invalid writes", [] {
        const auto parsed = statements(
            "CREATE TABLE account(id INT PRIMARY KEY,name VARCHAR(4) NOT NULL,active BOOL DEFAULT TRUE);"
            "INSERT INTO account(id,name) VALUES(1,'Ann');");
        MemoryCatalog catalog;
        const auto create = value(analyze(parsed[0], *catalog.snapshot()));
        const auto& definition = std::get<BoundCreateTable>(create.node);
        check(definition.columns[0].primary_key && definition.columns[0].not_null &&
              definition.columns[0].unique && definition.columns[1].varchar_length == 4,
              "CREATE constraint metadata was lost");
        value(catalog.createTable(definition.table_name, definition.columns));
        const auto insert = value(analyze(parsed[1], *catalog.snapshot()));
        const auto& row = std::get<BoundInsert>(insert.node).values;
        check(std::get<bool>(row[2]), "omitted column did not receive DEFAULT TRUE");
        failure(analyze(statements("INSERT INTO account(id) VALUES(2);")[0], *catalog.snapshot()),
                ErrorCode::MissingInsertColumn);
        failure(analyze(statements("INSERT INTO account VALUES(2,'ABCDE',FALSE);")[0],
                        *catalog.snapshot()), ErrorCode::TypeMismatch);
    });
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
    suite.run("A version2 scalar types and qualified names reach B plans", [] {
        MemoryCatalog catalog;
        const auto create_ast = statements(
            "CREATE TABLE metrics(id INT,active BOOL,score FLOAT,note VARCHAR);")[0];
        const auto create = value(analyze(create_ast, *catalog.snapshot()));
        value(buildPlan(create));
        const auto& definition = std::get<BoundCreateTable>(create.node);
        value(catalog.createTable(definition.table_name, definition.columns));

        auto parsed = statements("INSERT INTO metrics VALUES(1,TRUE,NULL,'first');");
        const auto insert = value(buildPlan(value(analyze(parsed[0], *catalog.snapshot()))));
        const auto& row = std::get<InsertPlan>(insert.root->node).values;
        check(std::get<bool>(row[1]) && std::holds_alternative<NullValue>(row[2]),
              "BOOL or NULL did not cross A/B boundary");

        parsed = statements(
            "SELECT metrics.id FROM metrics WHERE metrics.active=TRUE AND metrics.score>1.5;");
        const auto select = value(buildPlan(value(analyze(parsed[0], *catalog.snapshot()))));
        check(select.root->output[0].name == "id" &&
              formatPlan(select).find("1.5") != std::string::npos,
              "qualified name or FLOAT expression did not reach plan");
    });
    suite.run("JOIN GROUP BY ORDER BY cross the complete A/B pipeline", [] {
        Fixture f;
        value(f.catalog.createTable("score", {{"id", DataType::Int},
                                               {"student_id", DataType::Int},
                                               {"value", DataType::Int}}));
        const auto plan = f.compile(
            "SELECT student.name FROM student "
            "JOIN score ON student.id=score.student_id "
            "WHERE score.value>60 GROUP BY student.name ORDER BY student.name DESC;");
        const auto& project = std::get<ProjectPlan>(plan.root->node);
        const auto& sort = std::get<SortPlan>(project.input->node);
        const auto& group = std::get<GroupByPlan>(sort.input->node);
        const auto& filter = std::get<FilterPlan>(group.input->node);
        const auto& join = std::get<NestedLoopJoinPlan>(filter.input->node);
        check(std::holds_alternative<SeqScanPlan>(join.left->node) &&
              std::holds_alternative<SeqScanPlan>(join.right->node),
              "JOIN did not produce two scan inputs");
        check(group.keys.size() == 1 && sort.items[0].direction == SortDirection::Desc &&
              plan.root->output[0].name == "name", "GROUP/ORDER metadata was lost");

        const auto hidden_sort = f.compile("SELECT student.name FROM student ORDER BY student.age DESC;");
        check(std::holds_alternative<SortPlan>(std::get<ProjectPlan>(hidden_sort.root->node).input->node),
              "ORDER BY hidden column must run before projection");

        failure(f.bind("SELECT id FROM student JOIN score ON student.id=score.student_id;"),
                ErrorCode::AmbiguousColumn);
        failure(f.bind("SELECT student.id FROM student JOIN score ON student.id;"),
                ErrorCode::JoinConditionNotBoolean);
        failure(f.bind("SELECT student.name FROM student GROUP BY student.id;"),
                ErrorCode::InvalidGrouping);
        failure(f.bind("SELECT * FROM student WHERE NULL=NULL;"),
                ErrorCode::InvalidOperandType);
    });
    return suite.finish();
}
