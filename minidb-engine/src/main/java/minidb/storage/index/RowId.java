package minidb.storage.index;

/** Stable physical address of one row in a data page. */
public record RowId(int pageId, int slotId) {
    public RowId {
        if (pageId < 0) {
            throw new IllegalArgumentException("pageId must be non-negative");
        }
        if (slotId < 0) {
            throw new IllegalArgumentException("slotId must be non-negative");
        }
    }
}
