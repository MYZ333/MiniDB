package minidb.storage;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;

/** A physical database page. Its contents are opaque to this module. */
public final class Page {
    public static final int PAGE_SIZE = 4 * 1024;

    private final int pageId;
    private final byte[] data;

    public Page(int pageId) {
        this(pageId, new byte[PAGE_SIZE]);
    }

    public Page(int pageId, byte[] bytes) {
        if (pageId < 0) {
            throw new IllegalArgumentException("pageId must be non-negative");
        }
        if (bytes.length != PAGE_SIZE) {
            throw new IllegalArgumentException("page must contain exactly " + PAGE_SIZE + " bytes");
        }
        this.pageId = pageId;
        this.data = Arrays.copyOf(bytes, PAGE_SIZE);
    }

    public int pageId() {
        return pageId;
    }

    /**
     * Returns a new cursor over the same mutable page bytes. Cursor position is
     * private to the returned ByteBuffer, while writes modify this page.
     */
    public ByteBuffer data() {
        return ByteBuffer.wrap(data).order(ByteOrder.BIG_ENDIAN);
    }

    public byte[] copyBytes() {
        return Arrays.copyOf(data, PAGE_SIZE);
    }
}
