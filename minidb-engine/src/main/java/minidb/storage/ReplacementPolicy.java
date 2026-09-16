package minidb.storage;

/** Supported victim-selection policies for unpinned buffer frames. */
public enum ReplacementPolicy {
    LRU,
    FIFO
}
