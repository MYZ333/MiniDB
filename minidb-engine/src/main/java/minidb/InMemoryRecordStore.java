package minidb;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Temporary implementation used before the Java page/buffer storage system is available. */
public final class InMemoryRecordStore implements RecordStore {
    private final Map<Long, List<DatabaseEngine.StoredRow>> rows = new LinkedHashMap<>();
    private final Map<Long, Long> nextRowId = new LinkedHashMap<>();

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
    public void replace(long tableId, long rowId, List<Object> values) {
        List<DatabaseEngine.StoredRow> table = table(tableId);
        for (int i = 0; i < table.size(); i++) if (table.get(i).id() == rowId) { table.set(i, new DatabaseEngine.StoredRow(rowId, new ArrayList<>(values))); return; }
        throw new EngineException("StorageFailure", "row not found for replace");
    }
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
