package minidb.storage.index;

/** One key-to-row mapping returned by an index range scan. */
public record IndexEntry(int key, RowId rowId) {
    public IndexEntry {
        if (rowId == null) {
            throw new NullPointerException("rowId");
        }
    }
}
