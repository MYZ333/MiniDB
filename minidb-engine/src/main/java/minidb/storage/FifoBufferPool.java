package minidb.storage;

/** FIFO specialization with the same public BufferPool contract. */
public final class FifoBufferPool extends BufferPoolManager {
    public FifoBufferPool(int capacity, DiskManager diskManager) {
        super(capacity, diskManager, ReplacementPolicy.FIFO);
    }

    public FifoBufferPool(
            int capacity,
            DiskManager diskManager,
            BufferPoolEventListener eventListener) {
        super(capacity, diskManager, ReplacementPolicy.FIFO, eventListener);
    }
}
