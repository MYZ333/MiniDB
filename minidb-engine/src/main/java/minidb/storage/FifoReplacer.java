package minidb.storage;

import java.util.Iterator;
import java.util.LinkedHashSet;
import java.util.OptionalInt;
import java.util.Set;

/** FIFO order based on when a frame first becomes evictable. */
final class FifoReplacer implements Replacer {
    private final LinkedHashSet<Integer> arrivalOrder = new LinkedHashSet<>();
    private final Set<Integer> evictablePages = new java.util.HashSet<>();

    @Override
    public void recordLoad(int pageId) {
        arrivalOrder.add(pageId);
    }

    @Override
    public void setEvictable(int pageId, boolean evictable) {
        if (evictable) {
            evictablePages.add(pageId);
        } else {
            evictablePages.remove(pageId);
        }
    }

    @Override
    public OptionalInt evict() {
        Iterator<Integer> iterator = arrivalOrder.iterator();
        while (iterator.hasNext()) {
            int candidate = iterator.next();
            if (evictablePages.remove(candidate)) {
                return OptionalInt.of(candidate);
            }
        }
        return OptionalInt.empty();
    }

    @Override
    public void remove(int pageId) {
        arrivalOrder.remove(pageId);
        evictablePages.remove(pageId);
    }
}
