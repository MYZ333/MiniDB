package minidb;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Minimal dependency-free JSON reader for the C++ plan protocol. */
final class Json {
    private final String text;
    private int position;
    private Json(String text) { this.text = text; }

    static Object parse(String text) {
        Json parser = new Json(text);
        Object value = parser.value();
        parser.skipSpace();
        if (parser.position != text.length()) throw parser.error("unexpected trailing content");
        return value;
    }

    /** Serializes the JSON-compatible values used by the plan and Web API. */
    static String stringify(Object value) {
        StringBuilder output = new StringBuilder();
        write(value, output);
        return output.toString();
    }

    private static void write(Object value, StringBuilder output) {
        if (value == null) { output.append("null"); return; }
        if (value instanceof String text) { writeString(text, output); return; }
        if (value instanceof Boolean || value instanceof Number) { output.append(value); return; }
        if (value instanceof Map<?, ?> map) {
            output.append('{'); boolean first = true;
            for (Map.Entry<?, ?> entry : map.entrySet()) {
                if (!(entry.getKey() instanceof String key)) throw new IllegalArgumentException("JSON object key must be a string");
                if (!first) output.append(',');
                writeString(key, output); output.append(':'); write(entry.getValue(), output); first = false;
            }
            output.append('}'); return;
        }
        if (value instanceof Iterable<?> values) {
            output.append('['); boolean first = true;
            for (Object item : values) { if (!first) output.append(','); write(item, output); first = false; }
            output.append(']'); return;
        }
        throw new IllegalArgumentException("value is not JSON-compatible: " + value.getClass().getName());
    }

    private static void writeString(String text, StringBuilder output) {
        output.append('"');
        for (int i = 0; i < text.length(); i++) {
            char character = text.charAt(i);
            switch (character) {
                case '"' -> output.append("\\\""); case '\\' -> output.append("\\\\");
                case '\b' -> output.append("\\b"); case '\f' -> output.append("\\f");
                case '\n' -> output.append("\\n"); case '\r' -> output.append("\\r"); case '\t' -> output.append("\\t");
                default -> { if (character < 0x20) output.append(String.format("\\u%04x", (int) character)); else output.append(character); }
            }
        }
        output.append('"');
    }

    private Object value() {
        skipSpace();
        if (position >= text.length()) throw error("unexpected end of JSON");
        return switch (text.charAt(position)) {
            case '{' -> object(); case '[' -> array(); case '"' -> string();
            case 't' -> keyword("true", Boolean.TRUE); case 'f' -> keyword("false", Boolean.FALSE);
            case 'n' -> keyword("null", null); default -> number();
        };
    }
    private Map<String, Object> object() {
        expect('{'); Map<String, Object> result = new LinkedHashMap<>(); skipSpace();
        if (consume('}')) return result;
        while (true) {
            skipSpace(); if (position >= text.length() || text.charAt(position) != '"') throw error("object key must be a string");
            String key = string(); skipSpace(); expect(':'); result.put(key, value()); skipSpace();
            if (consume('}')) return result; expect(',');
        }
    }
    private List<Object> array() {
        expect('['); List<Object> result = new ArrayList<>(); skipSpace();
        if (consume(']')) return result;
        while (true) { result.add(value()); skipSpace(); if (consume(']')) return result; expect(','); }
    }
    private String string() {
        expect('"'); StringBuilder result = new StringBuilder();
        while (position < text.length()) {
            char ch = text.charAt(position++);
            if (ch == '"') return result.toString();
            if (ch != '\\') { result.append(ch); continue; }
            if (position >= text.length()) throw error("unfinished escape");
            char escaped = text.charAt(position++);
            switch (escaped) {
                case '"' -> result.append('"'); case '\\' -> result.append('\\'); case '/' -> result.append('/');
                case 'b' -> result.append('\b'); case 'f' -> result.append('\f'); case 'n' -> result.append('\n');
                case 'r' -> result.append('\r'); case 't' -> result.append('\t');
                case 'u' -> {
                    if (position + 4 > text.length()) throw error("unfinished unicode escape");
                    String digits = text.substring(position, position + 4);
                    try { result.append((char) Integer.parseInt(digits, 16)); } catch (NumberFormatException ex) { throw error("invalid unicode escape"); }
                    position += 4;
                }
                default -> throw error("invalid escape");
            }
        }
        throw error("unterminated string");
    }
    private Object number() {
        int start = position;
        if (consume('-')) { }
        int digits = position;
        while (position < text.length() && Character.isDigit(text.charAt(position))) position++;
        if (digits == position) throw error("invalid JSON value");
        if (position < text.length() && (text.charAt(position) == '.' || text.charAt(position) == 'e' || text.charAt(position) == 'E'))
            throw error("MiniDB plan JSON accepts integer numbers only");
        try { return Long.parseLong(text.substring(start, position)); } catch (NumberFormatException ex) { throw error("integer out of range"); }
    }
    private Object keyword(String word, Object value) {
        if (!text.startsWith(word, position)) throw error("invalid JSON value"); position += word.length(); return value;
    }
    private void expect(char expected) { skipSpace(); if (!consume(expected)) throw error("expected '" + expected + "'"); }
    private boolean consume(char expected) { if (position < text.length() && text.charAt(position) == expected) { position++; return true; } return false; }
    private void skipSpace() { while (position < text.length() && Character.isWhitespace(text.charAt(position))) position++; }
    private IllegalArgumentException error(String message) { return new IllegalArgumentException(message + " at JSON character " + position); }
}
