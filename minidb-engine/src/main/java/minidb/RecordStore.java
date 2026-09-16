package minidb;

import java.util.List;
import java.util.Optional;

/** Java storage-system integration boundary. Page storage will implement this interface later. */
public interface RecordStore {
    // Index metadata belongs to the durable catalog; the implementation owns page allocation.
    record IndexState(long id, long metadataPageId) { }
    record IndexRow(long key, long rowId) { }
    interface Index {
        void insert(long key, long rowId);
        boolean delete(long key);
        Optional<Long> search(long key);
        List<IndexRow> range(Long lower, boolean lowerInclusive, Long upper, boolean upperInclusive);
    }
    /** Persistent stores return the catalog recovered from their system pages. */
    record CatalogState(long version, long nextTableId, long nextIndexId,
                        List<DatabaseEngine.TableSchema> tables,
                        List<DatabaseEngine.IndexSchema> indexes) {
        public CatalogState(long version, long nextTableId, List<DatabaseEngine.TableSchema> tables) {
            this(version, nextTableId, 1, tables, List.of());
        }
    }

    default Optional<CatalogState> loadCatalog() { return Optional.empty(); }
    default void persistCatalog(CatalogState catalog) { }
    default IndexState createIndex(long indexId) { throw new EngineException("StorageFailure", "indexes are unavailable"); }
    default Index openIndex(long metadataPageId) { throw new EngineException("StorageFailure", "indexes are unavailable"); }
    default void dropIndex(long metadataPageId) { }
    void createTable(DatabaseEngine.TableSchema schema);
    void dropTable(long tableId);
    long insert(long tableId, List<Object> values);
    List<DatabaseEngine.StoredRow> scan(long tableId);
    long replace(long tableId, long rowId, List<Object> values);
    default Optional<DatabaseEngine.StoredRow> read(long tableId, long rowId) {
        return scan(tableId).stream().filter(row -> row.id() == rowId).findFirst();
    }
    void erase(long tableId, long rowId);
}
