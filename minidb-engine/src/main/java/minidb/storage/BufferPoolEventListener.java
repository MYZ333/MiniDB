package minidb.storage;

/** Receives optional diagnostic events without coupling storage to a logger. */
@FunctionalInterface
public interface BufferPoolEventListener {
    void onEvent(BufferPoolEvent event);

    static BufferPoolEventListener noOp() {
        return event -> {
        };
    }
}
