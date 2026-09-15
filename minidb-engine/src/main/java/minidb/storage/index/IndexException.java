package minidb.storage.index;

/** Checked exception for logical or on-disk B+ tree errors. */
public final class IndexException extends Exception {
    private final IndexErrorCode code;

    public IndexException(IndexErrorCode code, String message) {
        super(message);
        this.code = code;
    }

    public IndexErrorCode code() {
        return code;
    }
}
