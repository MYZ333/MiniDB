package minidb.storage;

/** Public byte-page interface consumed by the database engine. */
public interface BufferPool extends AutoCloseable {
    Page getPage(int pageId) throws StorageException;

    Page newPage() throws StorageException;

    /** Safer optional API that automatically pairs getPage with unpinPage. */
    default PageGuard getPageGuard(int pageId) throws StorageException {
        return PageGuard.fetch(this, pageId);
    }

    /** Safer optional API that automatically pairs newPage with unpinPage. */
    default PageGuard newPageGuard() throws StorageException {
        return PageGuard.allocate(this);
    }

    void unpinPage(int pageId, boolean dirty) throws StorageException;

    void flushPage(int pageId) throws StorageException;

    void flushAll() throws StorageException;

    void freePage(int pageId) throws StorageException;

    BufferPoolStats stats();

    @Override
    void close() throws StorageException;
}
