package minidb.storage;

import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.OptionalInt;

/** LRU order for frames whose pin count is zero. */
final class LruReplacer implements Replacer {
    private final LinkedHashMap<Integer, Boolean> evictablePages =
            new LinkedHashMap<>(16, 0.75f, true);

    @Override
    public void recordLoad(int pageId) {
        // LRU age starts when the first caller finishes using the page.
    }

    @Override
    public void setEvictable(int pageId, boolean evictable) {
        if (evictable) {
            evictablePages.put(pageId, Boolean.TRUE);
        } else {
            evictablePages.remove(pageId);
        }
    }

    @Override
    public OptionalInt evict() {
        Iterator<Integer> iterator = evictablePages.keySet().iterator();
        if (!iterator.hasNext()) {
            return OptionalInt.empty();
        }

        int victim = iterator.next();
        iterator.remove();
        return OptionalInt.of(victim);
    }

    @Override
    public void remove(int pageId) {
        evictablePages.remove(pageId);
    }
}
