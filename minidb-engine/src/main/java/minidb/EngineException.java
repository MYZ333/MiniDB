package minidb;

public final class EngineException extends RuntimeException {
    private final String code;
    private final Long line;
    private final Long column;

    public EngineException(String code, String message) { this(code, message, null, null); }
    public EngineException(String code, String message, Long line, Long column) {
        super(message); this.code = code; this.line = line; this.column = column;
    }
    public String code() { return code; }
    public Long line() { return line; }
    public Long column() { return column; }
}
