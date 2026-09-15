package minidb;

import minidb.storage.FileDiskManager;
import minidb.storage.LruBufferPool;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

/** Persistence regression for the D-layer slotted pages and system catalog. */
public final class PageRecordStoreTest {
    public static void main(String[] args) throws Exception {
        healsUnflushedEmptySuperblock();
        Path file = Files.createTempFile("minidb-page-store-", ".db");
        Files.deleteIfExists(file);
        try {
            try (PageRecordStore store = new PageRecordStore(new LruBufferPool(4, new FileDiskManager(file)))) {
                DatabaseEngine.TableSchema schema = new DatabaseEngine.TableSchema(1, "student", List.of(
                    new DatabaseEngine.ColumnSchema(1, "id", "INT"),
                    new DatabaseEngine.ColumnSchema(2, "score", "FLOAT"),
                    new DatabaseEngine.ColumnSchema(3, "name", "VARCHAR"),
                    new DatabaseEngine.ColumnSchema(4, "active", "BOOL")));
                store.createTable(schema);
                long row = store.insert(1, List.of(7L, 1.5d, "Alice", true));
                store.insert(1, java.util.Arrays.asList(8L, null, "Bob", false));
                store.replace(1, row, List.of(7L, 2.5d, "Alice expanded value", true));
                store.persistCatalog(new RecordStore.CatalogState(1, 2, List.of(schema)));
                check(store.scan(1).size() == 2, "rows must survive an in-place/moved update");
            }
            try (PageRecordStore store = new PageRecordStore(new LruBufferPool(4, new FileDiskManager(file)))) {
                RecordStore.CatalogState catalog = store.loadCatalog().orElseThrow();
                check(catalog.version() == 1 && catalog.tables().get(0).name().equals("student"), "catalog must survive restart");
                List<DatabaseEngine.StoredRow> rows = store.scan(1);
                check(rows.size() == 2 && rows.stream().anyMatch(row -> row.values().contains("Alice expanded value")), "typed rows must survive restart");
            }
            System.out.println("PageRecordStoreTest passed.");
        } finally {
            Files.deleteIfExists(file);
            Files.deleteIfExists(Path.of(file + ".alloc"));
        }
    }
    private static void healsUnflushedEmptySuperblock() throws Exception {
        Path file = Files.createTempFile("minidb-empty-superblock-", ".db");
        Files.deleteIfExists(file);
        try {
            try (LruBufferPool pool = new LruBufferPool(2, new FileDiskManager(file))) {
                try (var first = pool.newPageGuard(); var second = pool.newPageGuard()) {
                    check(first.pageId() == 0 && second.pageId() == 1, "blank pages should be allocated");
                }
            }
            try (PageRecordStore store = new PageRecordStore(new LruBufferPool(2, new FileDiskManager(file)))) {
                check(store.loadCatalog().isEmpty(), "a healed empty superblock has no catalog");
            }
            try (PageRecordStore store = new PageRecordStore(new LruBufferPool(2, new FileDiskManager(file)))) {
                check(store.loadCatalog().isEmpty(), "healed superblock must survive restart");
            }
        } finally {
            Files.deleteIfExists(file);
            Files.deleteIfExists(Path.of(file + ".alloc"));
        }
    }
    private static void check(boolean value, String message) { if (!value) throw new AssertionError(message); }
}
