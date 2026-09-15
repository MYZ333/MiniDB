package minidb.storage;

/** A structured diagnostic event emitted after a successful state change. */
public record BufferPoolEvent(Type type, int pageId) {
    public enum Type {
        HIT,
        MISS,
        ALLOCATE,
        EVICT,
        FLUSH,
        FREE
    }
}
