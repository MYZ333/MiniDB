package minidb.storage;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.Optional;
import java.util.Random;
import minidb.storage.index.BPlusTreeIndex;
import minidb.storage.index.IndexEntry;
import minidb.storage.index.IndexErrorCode;
import minidb.storage.index.IndexException;
import minidb.storage.index.RowId;

/** Dependency-free executable storage tests. */
public final class StorageSelfTest {
    private StorageSelfTest() {
    }

    public static void main(String[] args) throws Exception {
        run("disk pages survive restart", StorageSelfTest::diskPagesSurviveRestart);
        run("freed page ids are reused and cleared", StorageSelfTest::freedPagesAreReused);
        run("buffer pool records hits and evicts LRU", StorageSelfTest::bufferPoolUsesLru);
        run("all pinned frames reject another page", StorageSelfTest::allPinnedFramesRejectFetch);
        run("dirty flag is sticky until flush", StorageSelfTest::dirtyFlagIsSticky);
        run("pinned page cannot be freed", StorageSelfTest::pinnedPageCannotBeFreed);
        run("FIFO keeps arrival order after a cache hit", StorageSelfTest::fifoKeepsArrivalOrder);
        run("page guard unpins and flushes dirty data", StorageSelfTest::pageGuardUnpinsAndFlushes);
        run("buffer events expose cache activity", StorageSelfTest::bufferEventsAreObservable);
        run("B+ tree splits, searches, deletes and survives restart",
                StorageSelfTest::bPlusTreeSurvivesRestart);
        System.out.println("All storage tests passed.");
    }

    private static void diskPagesSurviveRestart() throws Exception {
        Path directory = Files.createTempDirectory("minidb-disk-test-");
        Path database = directory.resolve("data.db");
        try {
            int pageId;
            byte[] expected = pagePattern(37);
            try (DiskManager disk = new FileDiskManager(database)) {
                pageId = disk.allocatePage();
                equal(0, pageId, "first page id");
                disk.writePage(pageId, expected);
                disk.flush();
            }
            try (DiskManager disk = new FileDiskManager(database)) {
                arrayEqual(expected, disk.readPage(pageId), "page after restart");
            }
        } finally {
            deleteTree(directory);
        }
    }

    private static void freedPagesAreReused() throws Exception {
        Path directory = Files.createTempDirectory("minidb-free-test-");
        Path database = directory.resolve("data.db");
        try {
            try (DiskManager disk = new FileDiskManager(database)) {
                int first = disk.allocatePage();
                int second = disk.allocatePage();
                equal(0, first, "first allocated page");
                equal(1, second, "second allocated page");
                disk.writePage(first, pagePattern(91));
                disk.freePage(first);

                int reused = disk.allocatePage();
                equal(first, reused, "reused page id");
                arrayEqual(new byte[Page.PAGE_SIZE], disk.readPage(reused),
                        "reused page must be zero-filled");
            }
        } finally {
            deleteTree(directory);
        }
    }

    private static void bufferPoolUsesLru() throws Exception {
        Path directory = Files.createTempDirectory("minidb-lru-test-");
        Path database = directory.resolve("data.db");
        int page0;
        try {
            try (LruBufferPool pool = new LruBufferPool(2, new FileDiskManager(database))) {
                Page first = pool.newPage();
                page0 = first.pageId();
                pool.unpinPage(page0, false);

                Page second = pool.newPage();
                int page1 = second.pageId();
                pool.unpinPage(page1, false);

                Page firstAgain = pool.getPage(page0);
                firstAgain.data().put(0, (byte) 99);
                pool.unpinPage(page0, true);

                Page third = pool.newPage();
                int page2 = third.pageId();
                pool.unpinPage(page2, false);

                Page secondAgain = pool.getPage(page1);
                pool.unpinPage(secondAgain.pageId(), false);

                BufferPoolStats stats = pool.stats();
                equal(1L, stats.hits(), "buffer hits");
                equal(1L, stats.misses(), "buffer misses");
                equal(2L, stats.evictions(), "buffer evictions");
                equal(1L, stats.flushes(), "dirty victim flushes");
            }

            try (DiskManager disk = new FileDiskManager(database)) {
                equal((byte) 99, disk.readPage(page0)[0], "dirty page persisted after eviction");
            }
        } finally {
            deleteTree(directory);
        }
    }

    private static void allPinnedFramesRejectFetch() throws Exception {
        Path directory = Files.createTempDirectory("minidb-pinned-test-");
        Path database = directory.resolve("data.db");
        try {
            try (LruBufferPool pool = new LruBufferPool(1, new FileDiskManager(database))) {
                Page pinned = pool.newPage();
                StorageException error = expectStorageException(pool::newPage);
                equal(StorageErrorCode.NO_EVICTABLE_FRAME, error.code(),
                        "all-pinned error code");
                pool.unpinPage(pinned.pageId(), false);
            }
        } finally {
            deleteTree(directory);
        }
    }

    private static void dirtyFlagIsSticky() throws Exception {
        Path directory = Files.createTempDirectory("minidb-dirty-test-");
        Path database = directory.resolve("data.db");
        int pageId;
        try {
            try (LruBufferPool pool = new LruBufferPool(1, new FileDiskManager(database))) {
                Page page = pool.newPage();
                pageId = page.pageId();
                page.data().putInt(0, 123456);
                pool.unpinPage(pageId, true);

                Page samePage = pool.getPage(pageId);
                pool.unpinPage(samePage.pageId(), false);

                pool.flushAll();
                equal(1L, pool.stats().flushes(), "sticky dirty flag caused a flush");
            }

            try (DiskManager disk = new FileDiskManager(database)) {
                equal(123456, java.nio.ByteBuffer.wrap(disk.readPage(pageId)).getInt(0),
                        "flushed integer value");
            }
        } finally {
            deleteTree(directory);
        }
    }

    private static void pinnedPageCannotBeFreed() throws Exception {
        Path directory = Files.createTempDirectory("minidb-free-pinned-test-");
        Path database = directory.resolve("data.db");
        try {
            try (LruBufferPool pool = new LruBufferPool(1, new FileDiskManager(database))) {
                Page page = pool.newPage();
                StorageException error = expectStorageException(() -> pool.freePage(page.pageId()));
                equal(StorageErrorCode.PAGE_PINNED, error.code(), "free-pinned error code");
                pool.unpinPage(page.pageId(), false);
                pool.freePage(page.pageId());
            }
        } finally {
            deleteTree(directory);
        }
    }

    private static void fifoKeepsArrivalOrder() throws Exception {
        Path directory = Files.createTempDirectory("minidb-fifo-test-");
        Path database = directory.resolve("data.db");
        List<BufferPoolEvent> events = new ArrayList<>();
        try {
            try (FifoBufferPool pool = new FifoBufferPool(
                    2, new FileDiskManager(database), events::add)) {
                Page first = pool.newPage();
                int page0 = first.pageId();

                Page second = pool.newPage();
                pool.unpinPage(second.pageId(), false);
                // Unpin in reverse order: FIFO must still remember load order.
                pool.unpinPage(page0, false);

                // A hit must not make page0 younger under FIFO.
                Page firstAgain = pool.getPage(page0);
                pool.unpinPage(firstAgain.pageId(), false);

                Page third = pool.newPage();
                pool.unpinPage(third.pageId(), false);

                int firstVictim = events.stream()
                        .filter(event -> event.type() == BufferPoolEvent.Type.EVICT)
                        .findFirst()
                        .orElseThrow()
                        .pageId();
                equal(page0, firstVictim, "FIFO victim");
            }
        } finally {
            deleteTree(directory);
        }
    }

    private static void pageGuardUnpinsAndFlushes() throws Exception {
        Path directory = Files.createTempDirectory("minidb-guard-test-");
        Path database = directory.resolve("data.db");
        int pageId;
        try {
            try (BufferPool pool = new LruBufferPool(1, new FileDiskManager(database))) {
                PageGuard guard = pool.newPageGuard();
                pageId = guard.pageId();
                guard.data().putInt(0, 20260914);
                guard.markDirty();
                guard.close();
                guard.close(); // close is intentionally idempotent

                // Capacity is one, so this succeeds only if guard.close unpinned pageId.
                try (PageGuard ignored = pool.newPageGuard()) {
                    // No content change.
                }

                boolean rejectedAfterClose = false;
                try {
                    guard.data();
                } catch (IllegalStateException expected) {
                    rejectedAfterClose = true;
                }
                equal(true, rejectedAfterClose, "closed guard rejects access");
            }

            try (DiskManager disk = new FileDiskManager(database)) {
                equal(20260914, java.nio.ByteBuffer.wrap(disk.readPage(pageId)).getInt(0),
                        "guard dirty data persisted");
            }
        } finally {
            deleteTree(directory);
        }
    }

    private static void bufferEventsAreObservable() throws Exception {
        Path directory = Files.createTempDirectory("minidb-event-test-");
        Path database = directory.resolve("data.db");
        List<BufferPoolEvent.Type> eventTypes = new ArrayList<>();
        try {
            try (LruBufferPool pool = new LruBufferPool(
                    1,
                    new FileDiskManager(database),
                    event -> eventTypes.add(event.type()))) {
                Page page = pool.newPage();
                int pageId = page.pageId();
                page.data().put(0, (byte) 7);
                pool.unpinPage(pageId, true);

                Page hit = pool.getPage(pageId);
                pool.unpinPage(hit.pageId(), false);
                pool.flushPage(pageId);
                pool.freePage(pageId);
            }

            equal(List.of(
                            BufferPoolEvent.Type.ALLOCATE,
                            BufferPoolEvent.Type.HIT,
                            BufferPoolEvent.Type.FLUSH,
                            BufferPoolEvent.Type.FREE),
                    eventTypes,
                    "observable event order");
        } finally {
            deleteTree(directory);
        }
    }

    private static void bPlusTreeSurvivesRestart() throws Exception {
        Path directory = Files.createTempDirectory("minidb-index-test-");
        Path database = directory.resolve("data.db");
        int metadataPageId;
        try {
            try (BufferPool pool = new LruBufferPool(3, new FileDiskManager(database))) {
                BPlusTreeIndex index = BPlusTreeIndex.create(pool);
                metadataPageId = index.metadataPageId();

                List<Integer> keys = new ArrayList<>();
                for (int key = 0; key < 800; key++) {
                    keys.add(key);
                }
                Collections.shuffle(keys, new Random(20260915));
                for (int key : keys) {
                    index.insert(key, rowIdFor(key));
                }
                index.insert(Integer.MIN_VALUE, new RowId(9000, 1));
                index.insert(Integer.MAX_VALUE, new RowId(9000, 2));

                equal(802L, index.size(), "index size after inserts");
                for (int key = 0; key < 800; key++) {
                    equal(Optional.of(rowIdFor(key)), index.search(key),
                            "search key " + key);
                }
                equal(Optional.empty(), index.search(1000), "missing key");

                IndexException duplicate = expectIndexException(
                        () -> index.insert(42, new RowId(1, 1)));
                equal(IndexErrorCode.DUPLICATE_KEY, duplicate.code(),
                        "duplicate key error code");
                equal(802L, index.size(), "duplicate does not change size");

                for (int key = 0; key < 800; key += 3) {
                    equal(true, index.delete(key), "delete key " + key);
                }
                equal(false, index.delete(9999), "delete missing key");

                List<IndexEntry> range = index.range(95, 105);
                List<Long> actualKeys = range.stream().map(IndexEntry::key).toList();
                equal(List.of(95L, 97L, 98L, 100L, 101L, 103L, 104L), actualKeys,
                        "ordered range after deletes");
                pool.flushAll();
            }

            try (BufferPool pool = new FifoBufferPool(2, new FileDiskManager(database))) {
                BPlusTreeIndex reopened = BPlusTreeIndex.open(pool, metadataPageId);
                equal(535L, reopened.size(), "persisted index size");
                equal(Optional.empty(), reopened.search(300), "deleted key after restart");
                equal(Optional.of(rowIdFor(301)), reopened.search(301),
                        "existing key after restart");
                equal(Optional.of(new RowId(9000, 1)), reopened.search(Integer.MIN_VALUE),
                        "minimum integer key");
                equal(Optional.of(new RowId(9000, 2)), reopened.search(Integer.MAX_VALUE),
                        "maximum integer key");

                // Reinsert into a tree containing stale (safe) separators after lazy deletion.
                reopened.insert(300, new RowId(7000, 3));
                equal(Optional.of(new RowId(7000, 3)), reopened.search(300),
                        "reinsert deleted key");
            }
        } finally {
            deleteTree(directory);
        }
    }

    private static RowId rowIdFor(int key) {
        return new RowId(100 + key / 20, key % 20);
    }

    private static byte[] pagePattern(int seed) {
        byte[] data = new byte[Page.PAGE_SIZE];
        for (int i = 0; i < data.length; i++) {
            data[i] = (byte) (seed + i * 31);
        }
        return data;
    }

    private static StorageException expectStorageException(ThrowingAction action) throws Exception {
        try {
            action.run();
        } catch (StorageException e) {
            return e;
        }
        throw new AssertionError("expected StorageException");
    }

    private static IndexException expectIndexException(ThrowingAction action) throws Exception {
        try {
            action.run();
        } catch (IndexException e) {
            return e;
        }
        throw new AssertionError("expected IndexException");
    }

    private static void arrayEqual(byte[] expected, byte[] actual, String label) {
        if (!Arrays.equals(expected, actual)) {
            throw new AssertionError(label + ": byte arrays differ");
        }
    }

    private static void equal(Object expected, Object actual, String label) {
        if (!java.util.Objects.equals(expected, actual)) {
            throw new AssertionError(label + ": expected=" + expected + ", actual=" + actual);
        }
    }

    private static void run(String name, ThrowingAction test) throws Exception {
        test.run();
        System.out.println("PASS: " + name);
    }

    private static void deleteTree(Path root) throws IOException {
        if (!Files.exists(root)) {
            return;
        }
        try (var paths = Files.walk(root)) {
            for (Path path : paths.sorted(Comparator.reverseOrder()).toList()) {
                Files.deleteIfExists(path);
            }
        }
    }

    @FunctionalInterface
    private interface ThrowingAction {
        void run() throws Exception;
    }
}
