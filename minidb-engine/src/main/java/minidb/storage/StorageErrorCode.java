package minidb.storage;

/** Stable error categories shared with callers of the storage module. */
public enum StorageErrorCode {
    INVALID_PAGE_ID,
    PAGE_NOT_ALLOCATED,
    PAGE_NOT_CACHED,
    PAGE_PINNED,
    PAGE_NOT_PINNED,
    NO_EVICTABLE_FRAME,
    INVALID_PAGE_SIZE,
    CORRUPT_STORAGE,
    IO_ERROR,
    STORAGE_CLOSED
}
