package minidb.storage;

/** Fixed-size page I/O used internally by the buffer pool. */
public interface DiskManager extends AutoCloseable {
    int allocatePage() throws StorageException;

    void freePage(int pageId) throws StorageException;

    byte[] readPage(int pageId) throws StorageException;

    void writePage(int pageId, byte[] data) throws StorageException;

    void flush() throws StorageException;

    @Override
    void close() throws StorageException;
}
