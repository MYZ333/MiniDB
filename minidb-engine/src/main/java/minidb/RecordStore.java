package minidb;

import java.util.List;
import java.util.Optional;

/** Java storage-system integration boundary. Page storage will implement this interface later. */
public interface RecordStore {
    /** Persistent stores return the catalog recovered from their system pages. */
    record CatalogState(long version, long nextTableId, List<DatabaseEngine.TableSchema> tables) { }

    default Optional<CatalogState> loadCatalog() { return Optional.empty(); }
    default void persistCatalog(CatalogState catalog) { }
    void createTable(DatabaseEngine.TableSchema schema);
    void dropTable(long tableId);
    long insert(long tableId, List<Object> values);
    List<DatabaseEngine.StoredRow> scan(long tableId);
    void replace(long tableId, long rowId, List<Object> values);
    void erase(long tableId, long rowId);
}
