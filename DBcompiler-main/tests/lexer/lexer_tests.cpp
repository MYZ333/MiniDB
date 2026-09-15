// A 原有词法测试；合并时补充文件末尾注释和 CRLF/所有权回归。
#include "minisql/lexer.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace minisql;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

TokenStream lexOk(const std::string& sql) {
    auto result = lex(sql);
    if (const auto* error = std::get_if<Diagnostic>(&result)) {
        throw std::runtime_error("unexpected lexer error: " + error->message);
    }
    return std::get<TokenStream>(std::move(result));
}

void expectError(const std::string& sql, ErrorCode code) {
    auto result = lex(sql);
    const auto* error = std::get_if<Diagnostic>(&result);
    require(error != nullptr, "expected lexer error");
    require(error->stage == DiagnosticStage::Lexical, "expected lexical error stage");
    require(error->code == code, "unexpected lexer error code");
    require(error->span.has_value(), "lexer error should include source span");
}

void testBasicTokens() {
    const auto tokens = lexOk("SeLeCt name FROM student WHERE age >= 18 AND name != 'Tom''s book';");
    const std::vector<TokenKind> expected{
        TokenKind::Select, TokenKind::Identifier, TokenKind::From, TokenKind::Identifier,
        TokenKind::Where, TokenKind::Identifier, TokenKind::GreaterEqual, TokenKind::Integer,
        TokenKind::And, TokenKind::Identifier, TokenKind::NotEqual, TokenKind::String,
        TokenKind::Semicolon, TokenKind::EndOfInput};
    require(tokens.size() == expected.size(), "unexpected token count");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        require(tokens[i].kind == expected[i], "unexpected token kind");
    }
    require(tokens[0].lexeme == "SeLeCt", "keyword lexeme should keep original spelling");
    require(tokens[11].lexeme == "'Tom''s book'", "string lexeme should keep quotes");
}

void testTriviaAndPositions() {
    const auto tokens = lexOk("-- query\nSELECT\tname\n/* skip */FROM student;");
    require(tokens[0].kind == TokenKind::Select, "expected SELECT after line comment");
    require(tokens[0].span.begin.line == 2 && tokens[0].span.begin.column == 1,
            "SELECT should start on line 2 column 1");
    require(tokens[1].kind == TokenKind::Identifier && tokens[1].lexeme == "name",
            "expected identifier after tab");
    require(tokens[1].span.begin.line == 2 && tokens[1].span.begin.column == 8,
            "tab should count as one column");
    require(tokens[2].kind == TokenKind::From, "block comment should be skipped");
}

void testSingleCharacterTokens() {
    const auto tokens = lexOk("()+-*/,. ;= < > <= >= != <>");
    const std::vector<TokenKind> expected{
        TokenKind::LeftParen, TokenKind::RightParen, TokenKind::Plus, TokenKind::Minus,
        TokenKind::Star, TokenKind::Slash, TokenKind::Comma, TokenKind::Dot, TokenKind::Semicolon,
        TokenKind::Equal, TokenKind::Less, TokenKind::Greater, TokenKind::LessEqual,
        TokenKind::GreaterEqual, TokenKind::NotEqual, TokenKind::NotEqual, TokenKind::EndOfInput};
    require(tokens.size() == expected.size(), "unexpected operator token count");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        require(tokens[i].kind == expected[i], "unexpected operator token");
    }
}

void testExtendedTokens() {
    const auto tokens = lexOk(
        "SELECT student.id AS student_id FROM student s JOIN score AS x ON s.id=x.id "
        "GROUP BY student.id ORDER BY score.value DESC;"
        "CREATE TABLE metrics(active BOOL, value FLOAT);"
        "CREATE TABLE constrained(id INT PRIMARY KEY, name VARCHAR(20) NOT NULL UNIQUE DEFAULT 'x');"
        "INSERT INTO metrics VALUES(TRUE,3.14,NULL,FALSE);"
        "SELECT * FROM metrics WHERE value IS NOT NULL LIMIT 10 OFFSET 20;"
        "SELECT * FROM metrics WHERE note LIKE 'A%';"
        "SELECT COUNT(*),SUM(value),AVG(value),MIN(value),MAX(value) FROM metrics HAVING COUNT(*)>0;"
        "SELECT DISTINCT value FROM metrics;"
        "SELECT * FROM metrics WHERE value BETWEEN 10 AND 20;"
        "SELECT * FROM metrics WHERE value IN (1,2,3);"
        "ALTER TABLE student ADD COLUMN email VARCHAR(50);"
        "ALTER TABLE student RENAME COLUMN name TO full_name;"
        "DROP TABLE IF EXISTS old_student;"
        "SELECT id FROM a UNION ALL SELECT id FROM b INTERSECT SELECT id FROM c EXCEPT SELECT id FROM d;"
        "SELECT CASE WHEN active THEN 1 ELSE 0 END FROM metrics;"
        "SELECT * FROM a INNER JOIN b ON a.id=b.id LEFT OUTER JOIN c ON a.id=c.id "
        "RIGHT JOIN d ON a.id=d.id FULL OUTER JOIN e ON a.id=e.id;");
    const std::vector<TokenKind> required{TokenKind::Dot, TokenKind::Join, TokenKind::On,
        TokenKind::Group, TokenKind::Order, TokenKind::By, TokenKind::Desc, TokenKind::As, TokenKind::Bool,
        TokenKind::Float, TokenKind::True, TokenKind::FloatLiteral, TokenKind::Null, TokenKind::False,
        TokenKind::Is, TokenKind::Limit, TokenKind::Offset, TokenKind::Like,
        TokenKind::Between, TokenKind::In, TokenKind::Count, TokenKind::Sum, TokenKind::Avg, TokenKind::Min, TokenKind::Max,
        TokenKind::Primary, TokenKind::Key, TokenKind::Unique, TokenKind::Default,
        TokenKind::Alter, TokenKind::Add, TokenKind::Column, TokenKind::Rename, TokenKind::To,
        TokenKind::Drop, TokenKind::If, TokenKind::Exists,
        TokenKind::Having, TokenKind::Distinct, TokenKind::Inner, TokenKind::Left,
        TokenKind::Right, TokenKind::Full, TokenKind::Outer, TokenKind::Union,
        TokenKind::Intersect, TokenKind::Except, TokenKind::All,
        TokenKind::Case, TokenKind::When, TokenKind::Then, TokenKind::Else, TokenKind::End};
    for (const auto kind : required) {
        bool found = false;
        for (const auto& token : tokens) found = found || token.kind == kind;
        require(found, "extended token kind missing");
    }
    const auto explain = lexOk("EXPLAIN ANALYZE SELECT * FROM metrics;");
    require(explain[0].kind == TokenKind::Explain && explain[1].kind == TokenKind::Analyze,
            "EXPLAIN ANALYZE keywords are missing");
}

void testErrors() {
    expectError("@", ErrorCode::InvalidCharacter);
    expectError("'abc", ErrorCode::UnterminatedString);
    expectError("/* abc", ErrorCode::UnterminatedComment);
    expectError("a == b", ErrorCode::InvalidCharacter);
}

void testClosedCommentAtEof() {
    for (const std::string sql : {"/**/", "/* comment */", "SELECT * FROM t;/* tail */"}) {
        const auto tokens = lexOk(sql);
        require(tokens.back().kind == TokenKind::EndOfInput, "closed comment at EOF must be accepted");
        require(tokens.back().span.begin.offset == sql.size(), "EOF offset must include trailing comment");
    }
    expectError("/**/ /* missing", ErrorCode::UnterminatedComment);
}

void testCrLfAndOwnedText() {
    const auto tokens = lexOk(std::string("-- comment\r\nSELECT\tname;"));
    require(tokens[0].span.begin.offset == 12 && tokens[0].span.begin.line == 2 &&
            tokens[0].span.begin.column == 1, "CRLF must count as one newline and two bytes");
    require(tokens[1].lexeme == "name" && tokens[1].span.begin.column == 8,
            "token must own its text after temporary SQL is destroyed");
}

} // namespace

int main() {
    try {
        testBasicTokens();
        testTriviaAndPositions();
        testSingleCharacterTokens();
        testExtendedTokens();
        testErrors();
        testClosedCommentAtEof();
        testCrLfAndOwnedText();
        std::cout << "Lexer tests passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
