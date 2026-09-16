package minidb.storage;

import java.util.HashMap;
import java.util.Map;
import java.util.Objects;
import java.util.OptionalInt;

/**
 * Fixed-capacity buffer pool with configurable replacement, pin counts,
 * dirty-page writeback, statistics and optional structured events.
 */
public class BufferPoolManager implements BufferPool {
    private final int capacity;
    private final DiskManager diskManager;
    private final Map<Integer, BufferFrame> frames = new HashMap<>();
    private final Replacer replacer;
    private final BufferPoolEventListener eventListener;

    private long hits;
    private long misses;
    private long evictions;
    private long flushes;
    private boolean closed;

    public BufferPoolManager(
            int capacity,
            DiskManager diskManager,
            ReplacementPolicy replacementPolicy) {
        this(capacity, diskManager, replacementPolicy, BufferPoolEventListener.noOp());
    }

    public BufferPoolManager(
            int capacity,
            DiskManager diskManager,
            ReplacementPolicy replacementPolicy,
            BufferPoolEventListener eventListener) {
        if (capacity <= 0) {
            throw new IllegalArgumentException("buffer pool capacity must be positive");
        }
        this.capacity = capacity;
        this.diskManager = Objects.requireNonNull(diskManager, "diskManager");
        this.replacer = createReplacer(Objects.requireNonNull(
                replacementPolicy, "replacementPolicy"));
        this.eventListener = Objects.requireNonNull(eventListener, "eventListener");
    }

    @Override
    public synchronized Page getPage(int pageId) throws StorageException {
        ensureOpen("get_page");

        BufferFrame cached = frames.get(pageId);
        if (cached != null) {
            hits++;
            cached.pinCount++;
            replacer.setEvictable(pageId, false);
            emit(BufferPoolEvent.Type.HIT, pageId);
            return cached.page;
        }

        misses++;
        byte[] pageBytes = diskManager.readPage(pageId);
        makeRoom(pageId);

        Page page = new Page(pageId, pageBytes);
        frames.put(pageId, new BufferFrame(page, 1));
        replacer.recordLoad(pageId);
        emit(BufferPoolEvent.Type.MISS, pageId);
        return page;
    }

    @Override
    public synchronized Page newPage() throws StorageException {
        ensureOpen("new_page");
        makeRoom(null);

        int pageId = diskManager.allocatePage();
        Page page = new Page(pageId);
        frames.put(pageId, new BufferFrame(page, 1));
        replacer.recordLoad(pageId);
        emit(BufferPoolEvent.Type.ALLOCATE, pageId);
        return page;
    }

    @Override
    public synchronized void unpinPage(int pageId, boolean dirty) throws StorageException {
        ensureOpen("unpin_page");
        BufferFrame frame = requireCached(pageId, "unpin_page");
        if (frame.pinCount == 0) {
            throw error(StorageErrorCode.PAGE_NOT_PINNED, "unpin_page", pageId,
                    "page pin count is already zero");
        }

        frame.pinCount--;
        frame.dirty |= dirty;
        if (frame.pinCount == 0) {
            replacer.setEvictable(pageId, true);
        }
    }

    @Override
    public synchronized void flushPage(int pageId) throws StorageException {
        ensureOpen("flush_page");
        flushFrame(requireCached(pageId, "flush_page"));
    }

    @Override
    public synchronized void flushAll() throws StorageException {
        ensureOpen("flush_all");
        for (BufferFrame frame : frames.values()) {
            flushFrame(frame);
        }
        diskManager.flush();
    }

    @Override
    public synchronized void freePage(int pageId) throws StorageException {
        ensureOpen("free_page");
        BufferFrame frame = frames.get(pageId);
        if (frame != null && frame.pinCount > 0) {
            throw error(StorageErrorCode.PAGE_PINNED, "free_page", pageId,
                    "cannot free a pinned page");
        }

        diskManager.freePage(pageId);
        replacer.remove(pageId);
        frames.remove(pageId);
        emit(BufferPoolEvent.Type.FREE, pageId);
    }

    @Override
    public synchronized BufferPoolStats stats() {
        return new BufferPoolStats(hits, misses, evictions, flushes);
    }

    @Override
    public synchronized void close() throws StorageException {
        if (closed) {
            return;
        }

        StorageException failure = null;
        try {
            flushAll();
        } catch (StorageException e) {
            failure = e;
        }

        try {
            diskManager.close();
        } catch (StorageException e) {
            if (failure == null) {
                failure = e;
            } else {
                failure.addSuppressed(e);
            }
        } finally {
            closed = true;
            frames.clear();
        }

        if (failure != null) {
            throw failure;
        }
    }

    private void makeRoom(Integer requestedPageId) throws StorageException {
        if (frames.size() < capacity) {
            return;
        }

        OptionalInt victimResult = replacer.evict();
        if (victimResult.isEmpty()) {
            throw error(StorageErrorCode.NO_EVICTABLE_FRAME, "evict", requestedPageId,
                    "all buffer frames are pinned");
        }

        int victimPageId = victimResult.getAsInt();
        BufferFrame victim = frames.get(victimPageId);
        try {
            flushFrame(victim);
        } catch (StorageException e) {
            replacer.setEvictable(victimPageId, true);
            throw e;
        }

        replacer.remove(victimPageId);
        frames.remove(victimPageId);
        evictions++;
        emit(BufferPoolEvent.Type.EVICT, victimPageId);
    }

    private void flushFrame(BufferFrame frame) throws StorageException {
        if (!frame.dirty) {
            return;
        }
        diskManager.writePage(frame.page.pageId(), frame.page.copyBytes());
        frame.dirty = false;
        flushes++;
        emit(BufferPoolEvent.Type.FLUSH, frame.page.pageId());
    }

    private BufferFrame requireCached(int pageId, String operation) throws StorageException {
        BufferFrame frame = frames.get(pageId);
        if (frame == null) {
            throw error(StorageErrorCode.PAGE_NOT_CACHED, operation, pageId,
                    "page is not present in the buffer pool");
        }
        return frame;
    }

    private void ensureOpen(String operation) throws StorageException {
        if (closed) {
            throw error(StorageErrorCode.STORAGE_CLOSED, operation, null,
                    "buffer pool is closed");
        }
    }

    private void emit(BufferPoolEvent.Type type, int pageId) {
        try {
            eventListener.onEvent(new BufferPoolEvent(type, pageId));
        } catch (RuntimeException ignored) {
            // Diagnostics must never make a successful storage operation fail.
        }
    }

    private static Replacer createReplacer(ReplacementPolicy policy) {
        return switch (policy) {
            case LRU -> new LruReplacer();
            case FIFO -> new FifoReplacer();
        };
    }

    private static StorageException error(
            StorageErrorCode code,
            String operation,
            Integer pageId,
            String message) {
        return new StorageException(code, operation, pageId, message);
    }

    private static final class BufferFrame {
        private final Page page;
        private int pinCount;
        private boolean dirty;

        private BufferFrame(Page page, int pinCount) {
            this.page = page;
            this.pinCount = pinCount;
        }
    }
}
