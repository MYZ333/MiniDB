// A 负责：关键字、常量、注释、转义、多字符操作符以及源码位置追踪。
#include "minisql/lexer.hpp"

#include <cctype>
#include <string>
#include <unordered_map>
#include <utility>

namespace minisql {
namespace {

bool isIdentifierStart(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}

bool isIdentifierPart(char ch) {
    return isIdentifierStart(ch) || (ch >= '0' && ch <= '9');
}

std::string asciiLower(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return text;
}

TokenKind keywordOrIdentifier(const std::string& lexeme) {
    static const std::unordered_map<std::string, TokenKind> keywords{
        {"create", TokenKind::Create}, {"table", TokenKind::Table},
        {"insert", TokenKind::Insert}, {"into", TokenKind::Into},
        {"values", TokenKind::Values}, {"select", TokenKind::Select},
        {"from", TokenKind::From}, {"where", TokenKind::Where},
        {"update", TokenKind::Update}, {"set", TokenKind::Set},
        {"delete", TokenKind::Delete}, {"join", TokenKind::Join},
        {"on", TokenKind::On}, {"group", TokenKind::Group},
        {"order", TokenKind::Order}, {"by", TokenKind::By},
        {"asc", TokenKind::Asc}, {"desc", TokenKind::Desc}, {"as", TokenKind::As},
        {"int", TokenKind::Int}, {"varchar", TokenKind::Varchar},
        {"bool", TokenKind::Bool}, {"float", TokenKind::Float},
        {"null", TokenKind::Null}, {"true", TokenKind::True},
        {"false", TokenKind::False}, {"and", TokenKind::And},
        {"or", TokenKind::Or}, {"not", TokenKind::Not},
    };
    const auto found = keywords.find(asciiLower(lexeme));
    return found == keywords.end() ? TokenKind::Identifier : found->second;
}

class Lexer {
public:
    explicit Lexer(std::string_view sql) : sql_(sql) {}

    Result<TokenStream> run() {
        while (!atEnd()) {
            if (auto diagnostic = skipTrivia()) {
                return *diagnostic;
            }
            if (atEnd()) {
                break;
            }

            const SourcePosition start = position_;
            const char ch = peek();
            if (isIdentifierStart(ch)) {
                tokens_.push_back(identifier(start));
            } else if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
                tokens_.push_back(number(start));
            } else if (ch == '\'') {
                auto token = string(start);
                if (const auto* diagnostic = std::get_if<Diagnostic>(&token)) {
                    return *diagnostic;
                }
                tokens_.push_back(std::get<Token>(std::move(token)));
            } else {
                auto token = symbol(start);
                if (const auto* diagnostic = std::get_if<Diagnostic>(&token)) {
                    return *diagnostic;
                }
                tokens_.push_back(std::get<Token>(std::move(token)));
            }
        }

        tokens_.push_back(Token{TokenKind::EndOfInput, "", {position_, position_}});
        return tokens_;
    }

private:
    bool atEnd() const { return index_ >= sql_.size(); }
    char peek(std::size_t lookahead = 0) const {
        const std::size_t offset = index_ + lookahead;
        return offset < sql_.size() ? sql_[offset] : '\0';
    }

    char advance() {
        const char ch = sql_[index_++];
        ++position_.offset;
        if (ch == '\r') {
            if (peek() == '\n') {
                ++index_;
                ++position_.offset;
            }
            ++position_.line;
            position_.column = 1;
        } else if (ch == '\n') {
            ++position_.line;
            position_.column = 1;
        } else {
            ++position_.column;
        }
        return ch;
    }

    bool match(char expected) {
        if (peek() != expected) {
            return false;
        }
        advance();
        return true;
    }

    std::string textFrom(std::size_t start) const {
        return std::string(sql_.substr(start, index_ - start));
    }

    SourceSpan spanFrom(SourcePosition start) const { return {start, position_}; }

    Diagnostic diagnostic(ErrorCode code, std::string message, SourcePosition start) const {
        return Diagnostic{DiagnosticStage::Lexical, code, std::move(message),
                          SourceSpan{start, position_}};
    }

    std::optional<Diagnostic> skipTrivia() {
        bool consumed = true;
        while (consumed && !atEnd()) {
            consumed = false;
            while (!atEnd()) {
                const char ch = peek();
                if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                    advance();
                    consumed = true;
                } else {
                    break;
                }
            }
            if (peek() == '-' && peek(1) == '-') {
                while (!atEnd() && peek() != '\n' && peek() != '\r') {
                    advance();
                }
                consumed = true;
            } else if (peek() == '/' && peek(1) == '*') {
                const SourcePosition start = position_;
                bool closed = false;
                advance();
                advance();
                while (!atEnd()) {
                    if (peek() == '*' && peek(1) == '/') {
                        advance();
                        advance();
                        consumed = true;
                        closed = true;
                        break;
                    }
                    advance();
                }
                // 闭合符可能正好位于文件末尾；EOF 不等于注释未闭合。
                if (!closed) {
                    return diagnostic(ErrorCode::UnterminatedComment,
                                      "unterminated block comment", start);
                }
            }
        }
        return std::nullopt;
    }

    Token identifier(SourcePosition start) {
        const std::size_t begin = index_;
        while (!atEnd() && isIdentifierPart(peek())) {
            advance();
        }
        std::string lexeme = textFrom(begin);
        return Token{keywordOrIdentifier(lexeme), std::move(lexeme), spanFrom(start)};
    }

    Token number(SourcePosition start) {
        const std::size_t begin = index_;
        while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek())) != 0) {
            advance();
        }
        // 小数点两侧都必须有数字；否则把点留给限定列名或后续语法诊断。
        if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1))) != 0) {
            advance();
            while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek())) != 0) advance();
            return Token{TokenKind::FloatLiteral, textFrom(begin), spanFrom(start)};
        }
        return Token{TokenKind::Integer, textFrom(begin), spanFrom(start)};
    }

    Result<Token> string(SourcePosition start) {
        const std::size_t begin = index_;
        advance();
        while (!atEnd()) {
            if (peek() == '\n' || peek() == '\r') {
                return diagnostic(ErrorCode::UnterminatedString,
                                  "string literal cannot span lines", start);
            }
            if (peek() == '\'') {
                advance();
                if (peek() == '\'') {
                    advance();
                    continue;
                }
                return Token{TokenKind::String, textFrom(begin), spanFrom(start)};
            }
            advance();
        }
        return diagnostic(ErrorCode::UnterminatedString, "unterminated string literal", start);
    }

    Result<Token> symbol(SourcePosition start) {
        const std::size_t begin = index_;
        const char ch = advance();
        switch (ch) {
        case '=':
            if (match('=')) {
                return diagnostic(ErrorCode::InvalidCharacter, "'==' is not supported", start);
            }
            return Token{TokenKind::Equal, textFrom(begin), spanFrom(start)};
        case '!':
            if (match('=')) {
                return Token{TokenKind::NotEqual, textFrom(begin), spanFrom(start)};
            }
            return diagnostic(ErrorCode::InvalidCharacter, "expected '=' after '!'", start);
        case '<':
            if (match('=')) {
                return Token{TokenKind::LessEqual, textFrom(begin), spanFrom(start)};
            }
            if (match('>')) {
                return diagnostic(ErrorCode::InvalidCharacter, "'<>' is not supported", start);
            }
            return Token{TokenKind::Less, textFrom(begin), spanFrom(start)};
        case '>':
            if (match('=')) {
                return Token{TokenKind::GreaterEqual, textFrom(begin), spanFrom(start)};
            }
            return Token{TokenKind::Greater, textFrom(begin), spanFrom(start)};
        case '+': return Token{TokenKind::Plus, textFrom(begin), spanFrom(start)};
        case '-': return Token{TokenKind::Minus, textFrom(begin), spanFrom(start)};
        case '*': return Token{TokenKind::Star, textFrom(begin), spanFrom(start)};
        case '/': return Token{TokenKind::Slash, textFrom(begin), spanFrom(start)};
        case '(': return Token{TokenKind::LeftParen, textFrom(begin), spanFrom(start)};
        case ')': return Token{TokenKind::RightParen, textFrom(begin), spanFrom(start)};
        case ',': return Token{TokenKind::Comma, textFrom(begin), spanFrom(start)};
        case '.': return Token{TokenKind::Dot, textFrom(begin), spanFrom(start)};
        case ';': return Token{TokenKind::Semicolon, textFrom(begin), spanFrom(start)};
        default:
            return diagnostic(ErrorCode::InvalidCharacter,
                              std::string{"invalid character '"} + ch + "'", start);
        }
    }

    std::string_view sql_;
    std::size_t index_ = 0;
    SourcePosition position_{};
    TokenStream tokens_;
};

} // namespace

Result<TokenStream> lex(std::string_view sql) {
    Lexer lexer(sql);
    return lexer.run();
}

} // namespace minisql
