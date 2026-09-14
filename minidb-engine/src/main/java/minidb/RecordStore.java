package minidb;

import java.util.List;

/** Java storage-system integration boundary. Page storage will implement this interface later. */
public interface RecordStore {
    void createTable(DatabaseEngine.TableSchema schema);
    void dropTable(long tableId);
    long insert(long tableId, List<Object> values);
    List<DatabaseEngine.StoredRow> scan(long tableId);
    void replace(long tableId, long rowId, List<Object> values);
    void erase(long tableId, long rowId);
}
