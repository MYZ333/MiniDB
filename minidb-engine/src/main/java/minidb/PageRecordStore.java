package minidb;

import minidb.storage.BufferPool;
import minidb.storage.Page;
import minidb.storage.PageGuard;
import minidb.storage.StorageErrorCode;
import minidb.storage.StorageException;
import minidb.storage.index.BPlusTreeIndex;
import minidb.storage.index.IndexEntry;
import minidb.storage.index.IndexException;
import minidb.storage.index.RowId;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Base64;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;

/**
 * Database-engine storage adapter.  C owns page lifetime; this class owns the
 * catalog, slotted-page layout and typed row encoding.
 */
public final class PageRecordStore implements RecordStore, AutoCloseable {
    private static final int SUPER_MAGIC = 0x4D444253; // MDBS
    private static final int CATALOG_MAGIC = 0x4D444343; // MDCC
    private static final int DATA_MAGIC = 0x4D444450; // MDDP
    private static final int SUPER_SIZE = 16;
    private static final int CATALOG_HEADER = 12;
    private static final int DATA_HEADER = 16;
    private static final int SLOT_SIZE = 12;
    private final BufferPool pool;
    private final Map<Long, List<Integer>> pagesByTable = new LinkedHashMap<>();
    private int catalogRoot = -1;

    public PageRecordStore(BufferPool pool) {
        this.pool = pool;
        initialize();
    }

    @Override public Optional<CatalogState> loadCatalog() {
        if (catalogRoot < 0) return Optional.empty();
        try {
            byte[] bytes = readCatalog();
            @SuppressWarnings("unchecked") Map<String, Object> root = (Map<String, Object>) Json.parse(new String(bytes, StandardCharsets.UTF_8));
            pagesByTable.clear();
            for (Object raw : list(root.get("tablePages"))) {
                Map<String, Object> item = map(raw);
                List<Integer> pages = new ArrayList<>();
                for (Object page : list(item.get("pages"))) pages.add(Math.toIntExact(number(page)));
                pagesByTable.put(number(item.get("tableId")), pages);
            }
            List<DatabaseEngine.TableSchema> tables = new ArrayList<>();
            for (Object raw : list(root.get("tables"))) tables.add(decodeTable(map(raw)));
            List<DatabaseEngine.IndexSchema> indexes = new ArrayList<>();
            for (Object raw : listOrEmpty(root.get("indexes"))) indexes.add(decodeIndex(map(raw)));
            return Optional.of(new CatalogState(number(root.get("catalogVersion")), number(root.get("nextTableId")),
                root.get("nextIndexId") == null ? 1 : number(root.get("nextIndexId")), List.copyOf(tables), List.copyOf(indexes)));
        } catch (StorageException | RuntimeException error) {
            throw storage("loadCatalog", error);
        }
    }

    @Override public void persistCatalog(CatalogState state) {
        try {
            Map<String, Object> root = new LinkedHashMap<>();
            root.put("catalogVersion", state.version()); root.put("nextTableId", state.nextTableId()); root.put("nextIndexId", state.nextIndexId());
            List<Object> tables = new ArrayList<>();
            List<Object> tablePages = new ArrayList<>();
            for (DatabaseEngine.TableSchema table : state.tables()) {
                tables.add(encodeTable(table));
                tablePages.add(Map.of("tableId", table.id(), "pages", pagesByTable.getOrDefault(table.id(), List.of())));
            }
            root.put("tables", tables); root.put("tablePages", tablePages);
            List<Object> indexes = new ArrayList<>();
            for (DatabaseEngine.IndexSchema index : state.indexes()) indexes.add(encodeIndex(index));
            root.put("indexes", indexes);
            int oldRoot = catalogRoot;
            catalogRoot = writeCatalog(Json.stringify(root).getBytes(StandardCharsets.UTF_8));
            writeSuper(catalogRoot);
            freeCatalog(oldRoot);
        } catch (StorageException | RuntimeException error) {
            throw storage("persistCatalog", error);
        }
    }

    @Override public void createTable(DatabaseEngine.TableSchema schema) {
        if (pagesByTable.containsKey(schema.id())) throw new EngineException("StorageFailure", "record table already exists");
        try { pagesByTable.put(schema.id(), new ArrayList<>(List.of(newDataPage()))); }
        catch (StorageException error) { throw storage("createTable", error); }
    }

    @Override public void dropTable(long tableId) {
        List<Integer> pages = pagesByTable.remove(tableId);
        if (pages == null) throw new EngineException("StorageFailure", "record table not found: " + tableId);
        try { for (int page : pages) pool.freePage(page); }
        catch (StorageException error) { throw storage("dropTable", error); }
    }

    @Override public long insert(long tableId, List<Object> values) {
        byte[] row = encodeRow(values);
        List<Integer> pages = table(tableId);
        try {
            for (int pageId : pages) {
                int slot = append(pageId, row);
                if (slot >= 0) return pack(pageId, slot);
            }
            int pageId = newDataPage(); pages.add(pageId);
            int slot = append(pageId, row);
            if (slot < 0) throw new EngineException("StorageFailure", "row is too large for a database page");
            return pack(pageId, slot);
        } catch (StorageException error) { throw storage("insert", error); }
    }

    @Override public List<DatabaseEngine.StoredRow> scan(long tableId) {
        List<DatabaseEngine.StoredRow> rows = new ArrayList<>();
        try {
            for (int pageId : table(tableId)) try (PageGuard guard = pool.getPageGuard(pageId)) {
                ByteBuffer page = guard.data(); verifyData(page, pageId);
                int slots = page.getInt(12);
                for (int slot = 0; slot < slots; slot++) {
                    int base = DATA_HEADER + slot * SLOT_SIZE;
                    if (page.getInt(base + 8) != 1) continue;
                    int offset = page.getInt(base), length = page.getInt(base + 4);
                    byte[] bytes = new byte[length]; ByteBuffer copy = page.duplicate(); copy.position(offset); copy.get(bytes);
                    rows.add(new DatabaseEngine.StoredRow(pack(pageId, slot), decodeRow(bytes)));
                }
            }
            return rows;
        } catch (StorageException | RuntimeException error) { throw storage("scan", error); }
    }

    @Override public long replace(long tableId, long rowId, List<Object> values) {
        byte[] row = encodeRow(values); int pageId = pageId(rowId), slot = slotId(rowId);
        if (!table(tableId).contains(pageId)) throw new EngineException("StorageFailure", "row belongs to another table");
        boolean moved = false;
        try (PageGuard guard = pool.getPageGuard(pageId)) {
            ByteBuffer page = guard.data(); verifyData(page, pageId);
            int base = slotBase(page, slot);
            if (page.getInt(base + 8) != 1) throw new EngineException("StorageFailure", "row not found for replace");
            int oldLength = page.getInt(base + 4);
            if (row.length > oldLength) {
                // A slotted-page move is safe here: callers use the row id only for
                // the current mutation, and a later scan observes the replacement.
                page.putInt(base + 8, 0); guard.markDirty();
                moved = true;
            } else {
                ByteBuffer copy = page.duplicate(); copy.position(page.getInt(base)); copy.put(row);
                page.putInt(base + 4, row.length); guard.markDirty();
            }
        } catch (StorageException | RuntimeException error) { throw storage("replace", error); }
        return moved ? insert(tableId, values) : rowId;
    }

    @Override public Optional<DatabaseEngine.StoredRow> read(long tableId, long rowId) {
        int pageId = pageId(rowId), slot = slotId(rowId);
        if (!table(tableId).contains(pageId)) return Optional.empty();
        try (PageGuard guard = pool.getPageGuard(pageId)) {
            ByteBuffer page = guard.data(); verifyData(page, pageId);
            int base = slotBase(page, slot);
            if (page.getInt(base + 8) != 1) return Optional.empty();
            int offset = page.getInt(base), length = page.getInt(base + 4);
            byte[] bytes = new byte[length]; ByteBuffer copy = page.duplicate(); copy.position(offset); copy.get(bytes);
            return Optional.of(new DatabaseEngine.StoredRow(rowId, decodeRow(bytes)));
        } catch (StorageException | RuntimeException error) { throw storage("read", error); }
    }

    @Override public IndexState createIndex(long indexId) {
        try { BPlusTreeIndex index = BPlusTreeIndex.create(pool); return new IndexState(indexId, index.metadataPageId()); }
        catch (StorageException | IndexException error) { throw storage("createIndex", error); }
    }

    @Override public Index openIndex(long metadataPageId) {
        try {
            BPlusTreeIndex tree = BPlusTreeIndex.open(pool, Math.toIntExact(metadataPageId));
            return new Index() {
                public void insert(long key, long rowId) {
                    try { tree.insert(key, new RowId(pageId(rowId), slotId(rowId))); }
                    catch (StorageException | IndexException error) { throw storage("index insert", error); }
                }
                public boolean delete(long key) {
                    try { return tree.delete(key); }
                    catch (StorageException | IndexException error) { throw storage("index delete", error); }
                }
                public Optional<Long> search(long key) {
                    try { return tree.search(key).map(id -> pack(id.pageId(), id.slotId())); }
                    catch (StorageException | IndexException error) { throw storage("index search", error); }
                }
                public List<IndexRow> range(Long lower, boolean lowerInclusive, Long upper, boolean upperInclusive) {
                    long from = lower == null ? Long.MIN_VALUE : lower;
                    long to = upper == null ? Long.MAX_VALUE : upper;
                    try {
                        List<IndexRow> rows = new ArrayList<>();
                        for (IndexEntry entry : tree.range(from, to)) {
                            if (lower != null && !lowerInclusive && entry.key() == lower) continue;
                            if (upper != null && !upperInclusive && entry.key() == upper) continue;
                            rows.add(new IndexRow(entry.key(), pack(entry.rowId().pageId(), entry.rowId().slotId())));
                        }
                        return rows;
                    } catch (StorageException | IndexException error) { throw storage("index range", error); }
                }
            };
        } catch (StorageException | IndexException error) { throw storage("openIndex", error); }
    }

    @Override public void dropIndex(long metadataPageId) {
        try { BPlusTreeIndex.open(pool, Math.toIntExact(metadataPageId)).destroy(); }
        catch (StorageException | IndexException error) { throw storage("dropIndex", error); }
    }

    @Override public void erase(long tableId, long rowId) {
        int pageId = pageId(rowId), slot = slotId(rowId);
        if (!table(tableId).contains(pageId)) throw new EngineException("StorageFailure", "row belongs to another table");
        try (PageGuard guard = pool.getPageGuard(pageId)) {
            ByteBuffer page = guard.data(); verifyData(page, pageId);
            int base = slotBase(page, slot);
            if (page.getInt(base + 8) != 1) throw new EngineException("StorageFailure", "row not found for erase");
            page.putInt(base + 8, 0); guard.markDirty();
        } catch (StorageException | RuntimeException error) { throw storage("erase", error); }
    }

    public BufferPool bufferPool() { return pool; }
    @Override public void close() { try { pool.close(); } catch (StorageException error) { throw storage("close", error); } }

    private void initialize() {
        try {
            try (PageGuard guard = pool.getPageGuard(0)) {
                ByteBuffer page = guard.data();
                if (page.getInt(0) != SUPER_MAGIC) {
                    // A newly allocated page is written as zeroes before it enters the
                    // buffer pool. If the first process was interrupted before its
                    // initial flush, it is safe to finish this otherwise-empty setup.
                    if (!isZeroPage(page)) throw new EngineException("StorageFailure", "invalid MiniDB superblock");
                    initializeSuperblock(page); guard.markDirty(); return;
                }
                catalogRoot = page.getInt(8);
                return;
            } catch (StorageException missing) {
                if (missing.code() != StorageErrorCode.PAGE_NOT_ALLOCATED) throw missing;
                try (PageGuard guard = pool.newPageGuard()) {
                    if (guard.pageId() != 0) throw new EngineException("StorageFailure", "database does not begin with superblock page 0");
                    initializeSuperblock(guard.data()); guard.markDirty();
                }
            }
        } catch (StorageException error) { throw storage("initialize", error); }
    }

    private static void initializeSuperblock(ByteBuffer page) { page.putInt(0, SUPER_MAGIC); page.putInt(4, 1); page.putInt(8, -1); }
    private static boolean isZeroPage(ByteBuffer page) { for (int i = 0; i < Page.PAGE_SIZE; i++) if (page.get(i) != 0) return false; return true; }

    private int newDataPage() throws StorageException {
        try (PageGuard guard = pool.newPageGuard()) {
            ByteBuffer page = guard.data(); page.putInt(0, DATA_MAGIC); page.putInt(4, -1); page.putInt(8, Page.PAGE_SIZE); page.putInt(12, 0); guard.markDirty(); return guard.pageId();
        }
    }
    private int append(int pageId, byte[] row) throws StorageException {
        try (PageGuard guard = pool.getPageGuard(pageId)) {
            ByteBuffer page = guard.data(); verifyData(page, pageId); int slots = page.getInt(12), end = page.getInt(8);
            if (DATA_HEADER + (slots + 1) * SLOT_SIZE > end - row.length) return -1;
            int offset = end - row.length; ByteBuffer copy = page.duplicate(); copy.position(offset); copy.put(row);
            int base = DATA_HEADER + slots * SLOT_SIZE; page.putInt(base, offset); page.putInt(base + 4, row.length); page.putInt(base + 8, 1); page.putInt(8, offset); page.putInt(12, slots + 1); guard.markDirty(); return slots;
        }
    }
    private void verifyData(ByteBuffer page, int pageId) { if (page.getInt(0) != DATA_MAGIC) throw new EngineException("StorageFailure", "invalid data page " + pageId); }
    private int slotBase(ByteBuffer page, int slot) { int count = page.getInt(12); if (slot < 0 || slot >= count) throw new EngineException("StorageFailure", "row slot is out of range"); return DATA_HEADER + slot * SLOT_SIZE; }

    private void writeSuper(int root) throws StorageException { try (PageGuard guard = pool.getPageGuard(0)) { guard.data().putInt(8, root); guard.markDirty(); } }
    private int writeCatalog(byte[] bytes) throws StorageException {
        int first = -1, previous = -1, position = 0;
        do {
            try (PageGuard guard = pool.newPageGuard()) {
                ByteBuffer page = guard.data(); int count = Math.min(Page.PAGE_SIZE - CATALOG_HEADER, bytes.length - position);
                page.putInt(0, CATALOG_MAGIC); page.putInt(4, -1); page.putInt(8, count); if (count > 0) page.position(CATALOG_HEADER); page.put(bytes, position, count);
                if (previous >= 0) try (PageGuard prior = pool.getPageGuard(previous)) { prior.data().putInt(4, guard.pageId()); prior.markDirty(); }
                if (first < 0) first = guard.pageId(); previous = guard.pageId(); position += count; guard.markDirty();
            }
        } while (position < bytes.length); return first;
    }
    private byte[] readCatalog() throws StorageException {
        List<byte[]> parts = new ArrayList<>(); int total = 0, current = catalogRoot;
        while (current >= 0) try (PageGuard guard = pool.getPageGuard(current)) {
            ByteBuffer page = guard.data(); if (page.getInt(0) != CATALOG_MAGIC) throw new EngineException("StorageFailure", "invalid catalog page"); int count = page.getInt(8);
            if (count < 0 || count > Page.PAGE_SIZE - CATALOG_HEADER) throw new EngineException("StorageFailure", "invalid catalog length"); byte[] part = new byte[count]; page.position(CATALOG_HEADER); page.get(part); parts.add(part); total += count; current = page.getInt(4);
        }
        byte[] result = new byte[total]; int at = 0; for (byte[] part : parts) { System.arraycopy(part, 0, result, at, part.length); at += part.length; } return result;
    }
    private void freeCatalog(int root) throws StorageException { while (root >= 0) { int next; try (PageGuard guard = pool.getPageGuard(root)) { next = guard.data().getInt(4); } pool.freePage(root); root = next; } }
    private List<Integer> table(long id) { List<Integer> pages = pagesByTable.get(id); if (pages == null) throw new EngineException("StorageFailure", "record table not found: " + id); return pages; }

    private static byte[] encodeRow(List<Object> values) { java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream(); java.io.DataOutputStream out = new java.io.DataOutputStream(bytes); try { out.writeInt(values.size()); for (Object value : values) { if (value == null) out.writeByte(0); else if (value instanceof Long n) { out.writeByte(1); out.writeLong(n); } else if (value instanceof Double n) { out.writeByte(2); out.writeDouble(n); } else if (value instanceof String s) { byte[] text = s.getBytes(StandardCharsets.UTF_8); out.writeByte(3); out.writeInt(text.length); out.write(text); } else if (value instanceof Boolean b) { out.writeByte(4); out.writeBoolean(b); } else throw new EngineException("StorageFailure", "unsupported row value type"); } out.flush(); return bytes.toByteArray(); } catch (java.io.IOException impossible) { throw new AssertionError(impossible); } }
    private static List<Object> decodeRow(byte[] bytes) { try { java.io.DataInputStream in = new java.io.DataInputStream(new java.io.ByteArrayInputStream(bytes)); int count = in.readInt(); if (count < 0 || count > 4096) throw new EngineException("StorageFailure", "invalid row column count"); List<Object> values = new ArrayList<>(); for (int i = 0; i < count; i++) values.add(switch (in.readByte()) { case 0 -> null; case 1 -> in.readLong(); case 2 -> in.readDouble(); case 3 -> { int len = in.readInt(); if (len < 0 || len > Page.PAGE_SIZE) throw new EngineException("StorageFailure", "invalid string length"); yield new String(in.readNBytes(len), StandardCharsets.UTF_8); } case 4 -> in.readBoolean(); default -> throw new EngineException("StorageFailure", "invalid row value tag"); }); return java.util.Collections.unmodifiableList(values); } catch (java.io.IOException error) { throw new EngineException("StorageFailure", "corrupt row encoding", error); } }
    private static Map<String, Object> encodeTable(DatabaseEngine.TableSchema table) { Map<String, Object> result = new LinkedHashMap<>(); result.put("id", table.id()); result.put("name", table.name()); List<Object> columns = new ArrayList<>(); for (DatabaseEngine.ColumnSchema c : table.columns()) { Map<String, Object> column = new LinkedHashMap<>(); column.put("id", c.id()); column.put("name", c.name()); column.put("type", c.type()); column.put("varcharLength", c.varcharLength() == null ? -1L : c.varcharLength()); column.put("primaryKey", c.primaryKey()); column.put("notNull", c.notNull()); column.put("unique", c.unique()); column.put("default", c.defaultValue()); column.put("hasDefault", c.hasDefault()); columns.add(column); } List<Object> constraints = new ArrayList<>(); for (DatabaseEngine.TableConstraint c : table.constraints()) constraints.add(Map.of("kind", c.kind(), "columns", c.columns())); result.put("columns", columns); result.put("constraints", constraints); return result; }
    private static DatabaseEngine.TableSchema decodeTable(Map<String, Object> value) { List<DatabaseEngine.ColumnSchema> columns = new ArrayList<>(); for (Object raw : list(value.get("columns"))) { Map<String, Object> c = map(raw); long length = number(c.get("varcharLength")); columns.add(new DatabaseEngine.ColumnSchema(number(c.get("id")), string(c.get("name")), string(c.get("type")), length < 0 ? null : length, bool(c.get("primaryKey")), bool(c.get("notNull")), bool(c.get("unique")), c.get("default"), bool(c.get("hasDefault")))); } List<DatabaseEngine.TableConstraint> constraints = new ArrayList<>(); for (Object raw : list(value.get("constraints"))) { Map<String, Object> c = map(raw); List<Integer> members = new ArrayList<>(); for (Object member : list(c.get("columns"))) members.add(Math.toIntExact(number(member))); constraints.add(new DatabaseEngine.TableConstraint(string(c.get("kind")), List.copyOf(members))); } return new DatabaseEngine.TableSchema(number(value.get("id")), string(value.get("name")), List.copyOf(columns), List.copyOf(constraints)); }
    private static Map<String, Object> encodeIndex(DatabaseEngine.IndexSchema index) { return Map.of("id", index.id(), "name", index.name(), "tableId", index.tableId(), "columnId", index.columnId(), "keyType", index.keyType(), "unique", index.unique(), "metadataPageId", index.metadataPageId()); }
    private static DatabaseEngine.IndexSchema decodeIndex(Map<String, Object> value) { return new DatabaseEngine.IndexSchema(number(value.get("id")), string(value.get("name")), number(value.get("tableId")), number(value.get("columnId")), string(value.get("keyType")), bool(value.get("unique")), number(value.get("metadataPageId"))); }
    @SuppressWarnings("unchecked") private static Map<String, Object> map(Object value) { if (value instanceof Map<?, ?> map) return (Map<String, Object>) map; throw new EngineException("StorageFailure", "invalid catalog object"); }
    @SuppressWarnings("unchecked") private static List<Object> list(Object value) { if (value instanceof List<?> list) return (List<Object>) list; throw new EngineException("StorageFailure", "invalid catalog list"); }
    @SuppressWarnings("unchecked") private static List<Object> listOrEmpty(Object value) { return value == null ? List.of() : list(value); }
    private static String string(Object value) { if (value instanceof String text) return text; throw new EngineException("StorageFailure", "invalid catalog string"); }
    private static long number(Object value) { if (value instanceof Long n) return n; throw new EngineException("StorageFailure", "invalid catalog number"); }
    private static boolean bool(Object value) { return Boolean.TRUE.equals(value); }
    private static long pack(int pageId, int slotId) { return ((long) pageId << 32) | (slotId & 0xffffffffL); }
    private static int pageId(long rowId) { return (int) (rowId >>> 32); }
    private static int slotId(long rowId) { return (int) rowId; }
    private static EngineException storage(String operation, Throwable cause) { return cause instanceof EngineException engine ? engine : new EngineException("StorageFailure", operation + " failed: " + cause.getMessage(), cause); }
}
