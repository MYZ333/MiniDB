package minidb.storage;

/**
 * Checked exception used at the boundary between the storage system and the
 * database engine. The operation and page id make failures easy to diagnose.
 */
public final class StorageException extends Exception {
    private final StorageErrorCode code;
    private final String operation;
    private final Integer pageId;

    public StorageException(
            StorageErrorCode code,
            String operation,
            Integer pageId,
            String message) {
        super(message);
        this.code = code;
        this.operation = operation;
        this.pageId = pageId;
    }

    public StorageException(
            StorageErrorCode code,
            String operation,
            Integer pageId,
            String message,
            Throwable cause) {
        super(message, cause);
        this.code = code;
        this.operation = operation;
        this.pageId = pageId;
    }

    public StorageErrorCode code() {
        return code;
    }

    public String operation() {
        return operation;
    }

    public Integer pageId() {
        return pageId;
    }

    @Override
    public String getMessage() {
        String pagePart = pageId == null ? "" : ", pageId=" + pageId;
        return "StorageError{code=" + code
                + ", operation=" + operation
                + pagePart
                + ", reason=" + super.getMessage() + "}";
    }
}
