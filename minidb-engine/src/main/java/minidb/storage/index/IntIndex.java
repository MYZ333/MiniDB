package minidb.storage.index;

import java.util.List;
import java.util.Optional;
import minidb.storage.StorageException;

/** Unique, single-column integer index consumed by the database engine. */
public interface IntIndex {
    void insert(int key, RowId rowId) throws StorageException, IndexException;

    Optional<RowId> search(int key) throws StorageException, IndexException;

    List<IndexEntry> range(int fromInclusive, int toInclusive)
            throws StorageException, IndexException;

    boolean delete(int key) throws StorageException, IndexException;

    int metadataPageId();

    long size();
}
