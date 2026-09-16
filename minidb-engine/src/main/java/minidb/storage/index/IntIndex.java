package minidb.storage.index;

import java.util.List;
import java.util.Optional;
import minidb.storage.StorageException;

/** Unique, single-column integer index consumed by the database engine. */
public interface IntIndex {
    void insert(long key, RowId rowId) throws StorageException, IndexException;

    Optional<RowId> search(long key) throws StorageException, IndexException;

    List<IndexEntry> range(long fromInclusive, long toInclusive)
            throws StorageException, IndexException;

    boolean delete(long key) throws StorageException, IndexException;

    int metadataPageId();

    long size();
}
