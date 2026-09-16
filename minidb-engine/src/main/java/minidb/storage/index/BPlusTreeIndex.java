package minidb.storage.index;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.Optional;
import minidb.storage.BufferPool;
import minidb.storage.Page;
import minidb.storage.PageGuard;
import minidb.storage.StorageException;

/**
 * Persistent unique B+ tree from a signed 64-bit integer key to a {@link RowId}.
 *
 * <p>The metadata page id is stable. Root changes caused by splits are stored in
 * that page, so callers only need to persist {@link #metadataPageId()}.</p>
 */
public final class BPlusTreeIndex implements IntIndex {
    private static final int META_MAGIC = 0x4250544D; // BPTM
    private static final int NODE_MAGIC = 0x4250544E; // BPTN
    private static final int FORMAT_VERSION = 2;
    private static final int TYPE_LEAF = 1;
    private static final int TYPE_INTERNAL = 2;

    private static final int NODE_HEADER_SIZE = 20;
    private static final int LEAF_ENTRY_SIZE = Long.BYTES + 2 * Integer.BYTES;
    private static final int LEAF_MAX_KEYS =
            (Page.PAGE_SIZE - NODE_HEADER_SIZE) / LEAF_ENTRY_SIZE;
    private static final int INTERNAL_MAX_KEYS =
            (Page.PAGE_SIZE - NODE_HEADER_SIZE - Integer.BYTES) / (Long.BYTES + Integer.BYTES);

    private final BufferPool bufferPool;
    private final int metadataPageId;
    private int rootPageId;
    private long size;

    private BPlusTreeIndex(
            BufferPool bufferPool,
            int metadataPageId,
            int rootPageId,
            long size) {
        this.bufferPool = Objects.requireNonNull(bufferPool, "bufferPool");
        this.metadataPageId = metadataPageId;
        this.rootPageId = rootPageId;
        this.size = size;
    }

    /** Creates an empty index and returns its open handle. */
    public static BPlusTreeIndex create(BufferPool bufferPool)
            throws StorageException, IndexException {
        Objects.requireNonNull(bufferPool, "bufferPool");

        int metadataPageId;
        try (PageGuard metadata = bufferPool.newPageGuard()) {
            metadataPageId = metadata.pageId();
            writeMetadata(metadata.data(), -1, 0);
            metadata.markDirty();
        }

        int rootPageId;
        try {
            Node root = Node.leaf(-1);
            rootPageId = allocateNode(bufferPool, root);
        } catch (StorageException | IndexException e) {
            bufferPool.freePage(metadataPageId);
            throw e;
        }

        BPlusTreeIndex index = new BPlusTreeIndex(
                bufferPool, metadataPageId, rootPageId, 0);
        index.persistMetadata();
        return index;
    }

    /** Opens an existing index from the stable metadata page id stored by D. */
    public static BPlusTreeIndex open(BufferPool bufferPool, int metadataPageId)
            throws StorageException, IndexException {
        Objects.requireNonNull(bufferPool, "bufferPool");
        try (PageGuard metadata = bufferPool.getPageGuard(metadataPageId)) {
            ByteBuffer data = metadata.data();
            require(data.getInt(0) == META_MAGIC, "bad index metadata magic");
            require(data.getInt(4) == FORMAT_VERSION, "unsupported index format version");
            int rootPageId = data.getInt(8);
            long size = data.getLong(12);
            require(rootPageId >= 0, "invalid root page id");
            require(size >= 0, "negative index size");
            return new BPlusTreeIndex(bufferPool, metadataPageId, rootPageId, size);
        }
    }

    @Override
    public synchronized void insert(long key, RowId rowId)
            throws StorageException, IndexException {
        Objects.requireNonNull(rowId, "rowId");
        Split split = insertRecursive(rootPageId, key, rowId);
        if (split != null) {
            Node newRoot = Node.internal(-1);
            newRoot.children.add(rootPageId);
            newRoot.keys.add(split.separatorKey());
            newRoot.children.add(split.rightPageId());
            rootPageId = allocateNode(bufferPool, newRoot);
        }
        size++;
        persistMetadata();
    }

    @Override
    public synchronized Optional<RowId> search(long key)
            throws StorageException, IndexException {
        Node leaf = findLeaf(key);
        int position = lowerBound(leaf.keys, key);
        if (position < leaf.keys.size() && leaf.keys.get(position) == key) {
            return Optional.of(leaf.rowIds.get(position));
        }
        return Optional.empty();
    }

    @Override
    public synchronized List<IndexEntry> range(long fromInclusive, long toInclusive)
            throws StorageException, IndexException {
        if (fromInclusive > toInclusive) {
            return List.of();
        }

        List<IndexEntry> result = new ArrayList<>();
        Node leaf = findLeaf(fromInclusive);
        int position = lowerBound(leaf.keys, fromInclusive);

        while (true) {
            for (int i = position; i < leaf.keys.size(); i++) {
                long key = leaf.keys.get(i);
                if (key > toInclusive) {
                    return List.copyOf(result);
                }
                result.add(new IndexEntry(key, leaf.rowIds.get(i)));
            }
            if (leaf.nextLeafPageId < 0) {
                return List.copyOf(result);
            }
            leaf = readNode(leaf.nextLeafPageId);
            require(leaf.leaf, "leaf chain points to an internal node");
            position = 0;
        }
    }

    @Override
    public synchronized boolean delete(long key) throws StorageException, IndexException {
        if (search(key).isEmpty()) return false;
        // Rebuild the reachable tree after removal.  This deliberately trades deletion
        // throughput for a compact, fully balanced tree without leaving underfull nodes.
        List<IndexEntry> entries = range(Long.MIN_VALUE, Long.MAX_VALUE);
        freeSubtree(rootPageId);
        rootPageId = allocateNode(bufferPool, Node.leaf(-1));
        size = 0;
        for (IndexEntry entry : entries)
            if (entry.key() != key) insert(entry.key(), entry.rowId());
        persistMetadata();
        return true;
    }

    @Override
    public int metadataPageId() {
        return metadataPageId;
    }

    @Override
    public synchronized long size() {
        return size;
    }

    /** Releases every reachable node and the metadata page.  The caller must remove its catalog entry. */
    public synchronized void destroy() throws StorageException, IndexException {
        freeSubtree(rootPageId);
        bufferPool.freePage(metadataPageId);
    }

    private void freeSubtree(int pageId) throws StorageException, IndexException {
        Node node = readNode(pageId);
        if (!node.leaf) for (int child : node.children) freeSubtree(child);
        bufferPool.freePage(pageId);
    }

    private Split insertRecursive(int pageId, long key, RowId rowId)
            throws StorageException, IndexException {
        Node node = readNode(pageId);
        if (node.leaf) {
            int position = lowerBound(node.keys, key);
            if (position < node.keys.size() && node.keys.get(position) == key) {
                throw new IndexException(
                        IndexErrorCode.DUPLICATE_KEY,
                        "duplicate index key: " + key);
            }
            node.keys.add(position, key);
            node.rowIds.add(position, rowId);
            if (node.keys.size() <= LEAF_MAX_KEYS) {
                writeNode(node);
                return null;
            }
            return splitLeaf(node);
        }

        int childPosition = upperBound(node.keys, key);
        Split childSplit = insertRecursive(node.children.get(childPosition), key, rowId);
        if (childSplit == null) {
            return null;
        }

        node.keys.add(childPosition, childSplit.separatorKey());
        node.children.add(childPosition + 1, childSplit.rightPageId());
        if (node.keys.size() <= INTERNAL_MAX_KEYS) {
            writeNode(node);
            return null;
        }
        return splitInternal(node);
    }

    private Split splitLeaf(Node left) throws StorageException, IndexException {
        int splitPosition = left.keys.size() / 2;
        Node right = Node.leaf(-1);
        right.keys.addAll(left.keys.subList(splitPosition, left.keys.size()));
        right.rowIds.addAll(left.rowIds.subList(splitPosition, left.rowIds.size()));
        right.nextLeafPageId = left.nextLeafPageId;

        left.keys.subList(splitPosition, left.keys.size()).clear();
        left.rowIds.subList(splitPosition, left.rowIds.size()).clear();

        int rightPageId = allocateNode(bufferPool, right);
        left.nextLeafPageId = rightPageId;
        writeNode(left);
        return new Split(right.keys.get(0), rightPageId);
    }

    private Split splitInternal(Node left) throws StorageException, IndexException {
        int middle = left.keys.size() / 2;
        long separator = left.keys.get(middle);

        Node right = Node.internal(-1);
        right.keys.addAll(left.keys.subList(middle + 1, left.keys.size()));
        right.children.addAll(left.children.subList(middle + 1, left.children.size()));

        left.keys.subList(middle, left.keys.size()).clear();
        left.children.subList(middle + 1, left.children.size()).clear();

        int rightPageId = allocateNode(bufferPool, right);
        writeNode(left);
        return new Split(separator, rightPageId);
    }

    private Node findLeaf(long key) throws StorageException, IndexException {
        Node node = readNode(rootPageId);
        while (!node.leaf) {
            int childPosition = upperBound(node.keys, key);
            node = readNode(node.children.get(childPosition));
        }
        return node;
    }

    private Node readNode(int pageId) throws StorageException, IndexException {
        try (PageGuard guard = bufferPool.getPageGuard(pageId)) {
            return decodeNode(pageId, guard.data());
        }
    }

    private void writeNode(Node node) throws StorageException, IndexException {
        try (PageGuard guard = bufferPool.getPageGuard(node.pageId)) {
            encodeNode(node, guard.data());
            guard.markDirty();
        }
    }

    private static int allocateNode(BufferPool bufferPool, Node node)
            throws StorageException, IndexException {
        try (PageGuard guard = bufferPool.newPageGuard()) {
            node.pageId = guard.pageId();
            encodeNode(node, guard.data());
            guard.markDirty();
            return node.pageId;
        }
    }

    private void persistMetadata() throws StorageException {
        try (PageGuard metadata = bufferPool.getPageGuard(metadataPageId)) {
            writeMetadata(metadata.data(), rootPageId, size);
            metadata.markDirty();
        }
    }

    private static void writeMetadata(ByteBuffer data, int rootPageId, long size) {
        zero(data);
        data.putInt(0, META_MAGIC);
        data.putInt(4, FORMAT_VERSION);
        data.putInt(8, rootPageId);
        data.putLong(12, size);
    }

    private static Node decodeNode(int pageId, ByteBuffer data) throws IndexException {
        require(data.getInt(0) == NODE_MAGIC, "bad node magic at page " + pageId);
        require(data.getInt(4) == FORMAT_VERSION,
                "unsupported node format at page " + pageId);
        int type = data.getInt(8);
        int keyCount = data.getInt(12);
        int nextLeafPageId = data.getInt(16);
        require(type == TYPE_LEAF || type == TYPE_INTERNAL,
                "unknown node type at page " + pageId);

        boolean leaf = type == TYPE_LEAF;
        int maximum = leaf ? LEAF_MAX_KEYS : INTERNAL_MAX_KEYS;
        require(keyCount >= 0 && keyCount <= maximum,
                "invalid key count at page " + pageId);
        require(nextLeafPageId >= -1, "invalid next leaf page at page " + pageId);

        Node node = leaf ? Node.leaf(pageId) : Node.internal(pageId);
        node.nextLeafPageId = nextLeafPageId;
        int offset = NODE_HEADER_SIZE;
        if (leaf) {
            for (int i = 0; i < keyCount; i++) {
                long key = data.getLong(offset);
                int dataPageId = data.getInt(offset + Long.BYTES);
                int slotId = data.getInt(offset + Long.BYTES + Integer.BYTES);
                require(dataPageId >= 0 && slotId >= 0,
                        "invalid row id at page " + pageId);
                node.keys.add(key);
                node.rowIds.add(new RowId(dataPageId, slotId));
                offset += LEAF_ENTRY_SIZE;
            }
        } else {
            int firstChild = data.getInt(offset);
            require(firstChild >= 0, "invalid child page at page " + pageId);
            node.children.add(firstChild);
            offset += Integer.BYTES;
            for (int i = 0; i < keyCount; i++) {
                long key = data.getLong(offset);
                int child = data.getInt(offset + Long.BYTES);
                require(child >= 0, "invalid child page at page " + pageId);
                node.keys.add(key);
                node.children.add(child);
                offset += Long.BYTES + Integer.BYTES;
            }
        }
        validateSorted(node.keys, pageId);
        return node;
    }

    private static void encodeNode(Node node, ByteBuffer data) throws IndexException {
        int maximum = node.leaf ? LEAF_MAX_KEYS : INTERNAL_MAX_KEYS;
        require(node.keys.size() <= maximum, "node is too large");
        if (node.leaf) {
            require(node.rowIds.size() == node.keys.size(), "leaf entry count mismatch");
        } else {
            require(node.children.size() == node.keys.size() + 1,
                    "internal child count mismatch");
        }
        validateSorted(node.keys, node.pageId);

        zero(data);
        data.putInt(0, NODE_MAGIC);
        data.putInt(4, FORMAT_VERSION);
        data.putInt(8, node.leaf ? TYPE_LEAF : TYPE_INTERNAL);
        data.putInt(12, node.keys.size());
        data.putInt(16, node.leaf ? node.nextLeafPageId : -1);

        int offset = NODE_HEADER_SIZE;
        if (node.leaf) {
            for (int i = 0; i < node.keys.size(); i++) {
                RowId rowId = node.rowIds.get(i);
                data.putLong(offset, node.keys.get(i));
                data.putInt(offset + Long.BYTES, rowId.pageId());
                data.putInt(offset + Long.BYTES + Integer.BYTES, rowId.slotId());
                offset += LEAF_ENTRY_SIZE;
            }
        } else {
            data.putInt(offset, node.children.get(0));
            offset += Integer.BYTES;
            for (int i = 0; i < node.keys.size(); i++) {
                data.putLong(offset, node.keys.get(i));
                data.putInt(offset + Long.BYTES, node.children.get(i + 1));
                offset += Long.BYTES + Integer.BYTES;
            }
        }
    }

    private static void validateSorted(List<Long> keys, int pageId) throws IndexException {
        for (int i = 1; i < keys.size(); i++) {
            require(keys.get(i - 1) < keys.get(i),
                    "keys are not strictly sorted at page " + pageId);
        }
    }

    private static int lowerBound(List<Long> keys, long target) {
        int low = 0;
        int high = keys.size();
        while (low < high) {
            int middle = (low + high) >>> 1;
            if (keys.get(middle) < target) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        return low;
    }

    private static int upperBound(List<Long> keys, long target) {
        int low = 0;
        int high = keys.size();
        while (low < high) {
            int middle = (low + high) >>> 1;
            if (keys.get(middle) <= target) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        return low;
    }

    private static void zero(ByteBuffer data) {
        for (int i = 0; i < Page.PAGE_SIZE; i++) {
            data.put(i, (byte) 0);
        }
    }

    private static void require(boolean condition, String message) throws IndexException {
        if (!condition) {
            throw new IndexException(IndexErrorCode.CORRUPT_INDEX, message);
        }
    }

    private record Split(long separatorKey, int rightPageId) {
    }

    private static final class Node {
        private int pageId;
        private final boolean leaf;
        private final List<Long> keys = new ArrayList<>();
        private final List<RowId> rowIds = new ArrayList<>();
        private final List<Integer> children = new ArrayList<>();
        private int nextLeafPageId = -1;

        private Node(int pageId, boolean leaf) {
            this.pageId = pageId;
            this.leaf = leaf;
        }

        private static Node leaf(int pageId) {
            return new Node(pageId, true);
        }

        private static Node internal(int pageId) {
            return new Node(pageId, false);
        }
    }
}
