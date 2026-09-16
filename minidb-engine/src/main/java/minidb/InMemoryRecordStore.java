package minidb;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.NavigableMap;
import java.util.Optional;
import java.util.TreeMap;

/** Temporary implementation used before the Java page/buffer storage system is available. */
public final class InMemoryRecordStore implements RecordStore {
    private final Map<Long, List<DatabaseEngine.StoredRow>> rows = new LinkedHashMap<>();
    private final Map<Long, Long> nextRowId = new LinkedHashMap<>();
    private final Map<Long, NavigableMap<Long, Long>> indexes = new LinkedHashMap<>();
    private long nextIndexPage = 1;

    public void createTable(DatabaseEngine.TableSchema schema) {
        if (rows.putIfAbsent(schema.id(), new ArrayList<>()) != null) throw new EngineException("StorageFailure", "record table already exists");
        nextRowId.put(schema.id(), 1L);
    }
    public void dropTable(long tableId) {
        if (rows.remove(tableId) == null)
            throw new EngineException("StorageFailure", "record table not found: " + tableId);
        nextRowId.remove(tableId);
    }
    public long insert(long tableId, List<Object> values) {
        List<DatabaseEngine.StoredRow> table = table(tableId); long rowId = nextRowId.compute(tableId, (id, old) -> old + 1) - 1;
        table.add(new DatabaseEngine.StoredRow(rowId, new ArrayList<>(values))); return rowId;
    }
    public List<DatabaseEngine.StoredRow> scan(long tableId) {
        List<DatabaseEngine.StoredRow> snapshot = new ArrayList<>();
        for (DatabaseEngine.StoredRow row : table(tableId)) snapshot.add(new DatabaseEngine.StoredRow(row.id(), new ArrayList<>(row.values())));
        return snapshot;
    }
    public long replace(long tableId, long rowId, List<Object> values) {
        List<DatabaseEngine.StoredRow> table = table(tableId);
        for (int i = 0; i < table.size(); i++) if (table.get(i).id() == rowId) { table.set(i, new DatabaseEngine.StoredRow(rowId, new ArrayList<>(values))); return rowId; }
        throw new EngineException("StorageFailure", "row not found for replace");
    }
    @Override public Optional<DatabaseEngine.StoredRow> read(long tableId, long rowId) {
        return table(tableId).stream().filter(row -> row.id() == rowId).findFirst();
    }
    @Override public IndexState createIndex(long indexId) {
        long metadata = nextIndexPage++;
        indexes.put(metadata, new TreeMap<>());
        return new IndexState(indexId, metadata);
    }
    @Override public Index openIndex(long metadataPageId) {
        NavigableMap<Long, Long> map = indexes.get(metadataPageId);
        if (map == null) throw new EngineException("StorageFailure", "index not found: " + metadataPageId);
        return new Index() {
            public void insert(long key, long rowId) {
                if (map.putIfAbsent(key, rowId) != null) throw new EngineException("ConstraintViolation", "duplicate index key: " + key);
            }
            public boolean delete(long key) { return map.remove(key) != null; }
            public Optional<Long> search(long key) { return Optional.ofNullable(map.get(key)); }
            public List<IndexRow> range(Long lower, boolean lowerInclusive, Long upper, boolean upperInclusive) {
                NavigableMap<Long, Long> view = map;
                if (lower != null) view = view.tailMap(lower, lowerInclusive);
                if (upper != null) view = view.headMap(upper, upperInclusive);
                List<IndexRow> rows = new ArrayList<>();
                view.forEach((key, rowId) -> rows.add(new IndexRow(key, rowId)));
                return rows;
            }
        };
    }
    @Override public void dropIndex(long metadataPageId) { indexes.remove(metadataPageId); }
    public void erase(long tableId, long rowId) {
        List<DatabaseEngine.StoredRow> table = table(tableId);
        if (!table.removeIf(row -> row.id() == rowId)) throw new EngineException("StorageFailure", "row not found for erase");
    }
    private List<DatabaseEngine.StoredRow> table(long tableId) {
        List<DatabaseEngine.StoredRow> table = rows.get(tableId);
        if (table == null) throw new EngineException("StorageFailure", "record table not found: " + tableId);
        return table;
    }
}
