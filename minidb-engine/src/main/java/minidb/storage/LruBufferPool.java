package minidb.storage;

/** Backward-compatible LRU specialization of the configurable buffer pool. */
public final class LruBufferPool extends BufferPoolManager {
    public LruBufferPool(int capacity, DiskManager diskManager) {
        super(capacity, diskManager, ReplacementPolicy.LRU);
    }

    public LruBufferPool(
            int capacity,
            DiskManager diskManager,
            BufferPoolEventListener eventListener) {
        super(capacity, diskManager, ReplacementPolicy.LRU, eventListener);
    }
}
