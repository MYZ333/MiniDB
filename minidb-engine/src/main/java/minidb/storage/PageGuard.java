package minidb.storage;

import java.nio.ByteBuffer;
import java.util.Objects;

/**
 * A scoped page reference. Closing the guard unpins the page exactly once.
 * Call {@link #markDirty()} after changing bytes through {@link #data()}.
 */
public final class PageGuard implements AutoCloseable {
    private final BufferPool bufferPool;
    private final Page page;
    private boolean dirty;
    private boolean closed;

    static PageGuard fetch(BufferPool bufferPool, int pageId) throws StorageException {
        Objects.requireNonNull(bufferPool, "bufferPool");
        return new PageGuard(bufferPool, bufferPool.getPage(pageId));
    }

    static PageGuard allocate(BufferPool bufferPool) throws StorageException {
        Objects.requireNonNull(bufferPool, "bufferPool");
        return new PageGuard(bufferPool, bufferPool.newPage());
    }

    private PageGuard(BufferPool bufferPool, Page page) {
        this.bufferPool = bufferPool;
        this.page = page;
    }

    public Page page() {
        ensureOpen();
        return page;
    }

    public int pageId() {
        ensureOpen();
        return page.pageId();
    }

    public ByteBuffer data() {
        ensureOpen();
        return page.data();
    }

    public void markDirty() {
        ensureOpen();
        dirty = true;
    }

    public boolean isDirty() {
        ensureOpen();
        return dirty;
    }

    @Override
    public void close() throws StorageException {
        if (closed) {
            return;
        }
        closed = true;
        bufferPool.unpinPage(page.pageId(), dirty);
    }

    private void ensureOpen() {
        if (closed) {
            throw new IllegalStateException("page guard is already closed");
        }
    }
}
