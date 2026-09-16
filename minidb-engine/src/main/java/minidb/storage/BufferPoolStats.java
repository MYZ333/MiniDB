package minidb.storage;

/** Immutable snapshot of observable buffer-pool activity. */
public record BufferPoolStats(long hits, long misses, long evictions, long flushes) {
}
