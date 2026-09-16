package minidb.storage;

import java.util.OptionalInt;

/** Chooses an unpinned buffer frame when the buffer pool is full. */
interface Replacer {
    void recordLoad(int pageId);

    void setEvictable(int pageId, boolean evictable);

    OptionalInt evict();

    void remove(int pageId);
}
