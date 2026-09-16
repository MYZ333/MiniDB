package minidb.storage;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.EOFException;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.util.NavigableSet;
import java.util.TreeSet;

/**
 * Stores pages in one data file and allocation state in a small sidecar file.
 * Page N starts at N * 4096 in the data file.
 */
public final class FileDiskManager implements DiskManager {
    private static final int ALLOCATION_MAGIC = 0x4D444241; // "MDBA"
    private static final int ALLOCATION_VERSION = 1;

    private final Path dataPath;
    private final Path allocationPath;
    private final FileChannel channel;
    private final NavigableSet<Integer> freePageIds = new TreeSet<>();

    private int nextPageId;
    private boolean closed;

    public FileDiskManager(Path dataPath) throws StorageException {
        java.util.Objects.requireNonNull(dataPath, "dataPath");
        this.dataPath = dataPath.toAbsolutePath().normalize();
        this.allocationPath = this.dataPath.resolveSibling(
                this.dataPath.getFileName().toString() + ".alloc");

        try {
            Path parent = this.dataPath.getParent();
            if (parent != null) {
                Files.createDirectories(parent);
            }
            this.channel = FileChannel.open(
                    this.dataPath,
                    StandardOpenOption.CREATE,
                    StandardOpenOption.READ,
                    StandardOpenOption.WRITE);
            loadAllocationState();
        } catch (IOException e) {
            closeAfterFailedOpen();
            throw error(StorageErrorCode.IO_ERROR, "open", null,
                    "cannot open database file " + this.dataPath, e);
        } catch (StorageException e) {
            closeAfterFailedOpen();
            throw e;
        }
    }

    @Override
    public synchronized int allocatePage() throws StorageException {
        ensureOpen("allocate_page");

        boolean reused = !freePageIds.isEmpty();
        int pageId = reused ? freePageIds.pollFirst() : nextPageId++;

        try {
            writeRawPage(pageId, new byte[Page.PAGE_SIZE]);
            persistAllocationState();
            return pageId;
        } catch (StorageException e) {
            if (reused) {
                freePageIds.add(pageId);
            } else {
                nextPageId--;
            }
            throw e;
        }
    }

    @Override
    public synchronized void freePage(int pageId) throws StorageException {
        ensureOpen("free_page");
        ensureAllocated(pageId, "free_page");

        freePageIds.add(pageId);
        try {
            persistAllocationState();
        } catch (StorageException e) {
            freePageIds.remove(pageId);
            throw e;
        }
    }

    @Override
    public synchronized byte[] readPage(int pageId) throws StorageException {
        ensureOpen("read_page");
        ensureAllocated(pageId, "read_page");

        ByteBuffer buffer = ByteBuffer.allocate(Page.PAGE_SIZE);
        long offset = pageOffset(pageId);

        try {
            while (buffer.hasRemaining()) {
                int count = channel.read(buffer, offset + buffer.position());
                if (count < 0) {
                    throw error(StorageErrorCode.CORRUPT_STORAGE, "read_page", pageId,
                            "page is shorter than " + Page.PAGE_SIZE + " bytes");
                }
                if (count == 0) {
                    throw error(StorageErrorCode.IO_ERROR, "read_page", pageId,
                            "page read made no progress");
                }
            }
            return buffer.array();
        } catch (IOException e) {
            throw error(StorageErrorCode.IO_ERROR, "read_page", pageId,
                    "cannot read page", e);
        }
    }

    @Override
    public synchronized void writePage(int pageId, byte[] data) throws StorageException {
        ensureOpen("write_page");
        ensureAllocated(pageId, "write_page");
        if (data.length != Page.PAGE_SIZE) {
            throw error(StorageErrorCode.INVALID_PAGE_SIZE, "write_page", pageId,
                    "expected " + Page.PAGE_SIZE + " bytes but received " + data.length);
        }
        writeRawPage(pageId, data);
    }

    @Override
    public synchronized void flush() throws StorageException {
        ensureOpen("flush");
        try {
            channel.force(true);
            persistAllocationState();
        } catch (IOException e) {
            throw error(StorageErrorCode.IO_ERROR, "flush", null,
                    "cannot flush database file", e);
        }
    }

    @Override
    public synchronized void close() throws StorageException {
        if (closed) {
            return;
        }

        StorageException failure = null;
        try {
            flush();
        } catch (StorageException e) {
            failure = e;
        }

        try {
            channel.close();
        } catch (IOException e) {
            if (failure == null) {
                failure = error(StorageErrorCode.IO_ERROR, "close", null,
                        "cannot close database file", e);
            } else {
                failure.addSuppressed(e);
            }
        } finally {
            closed = true;
        }

        if (failure != null) {
            throw failure;
        }
    }

    private void loadAllocationState() throws IOException, StorageException {
        long fileSize = channel.size();
        if (fileSize % Page.PAGE_SIZE != 0) {
            throw error(StorageErrorCode.CORRUPT_STORAGE, "open", null,
                    "database file size is not a multiple of " + Page.PAGE_SIZE);
        }

        int pageCountFromFile = Math.toIntExact(fileSize / Page.PAGE_SIZE);
        if (!Files.exists(allocationPath)) {
            nextPageId = pageCountFromFile;
            persistAllocationState();
            return;
        }

        try (DataInputStream input = new DataInputStream(Files.newInputStream(allocationPath))) {
            int magic = input.readInt();
            int version = input.readInt();
            int storedNextPageId = input.readInt();
            int freeCount = input.readInt();

            if (magic != ALLOCATION_MAGIC || version != ALLOCATION_VERSION) {
                throw error(StorageErrorCode.CORRUPT_STORAGE, "open", null,
                        "unsupported allocation metadata");
            }
            if (storedNextPageId < 0 || storedNextPageId != pageCountFromFile) {
                throw error(StorageErrorCode.CORRUPT_STORAGE, "open", null,
                        "allocation metadata does not match data file size");
            }
            if (freeCount < 0 || freeCount > storedNextPageId) {
                throw error(StorageErrorCode.CORRUPT_STORAGE, "open", null,
                        "invalid free page count");
            }

            nextPageId = storedNextPageId;
            for (int i = 0; i < freeCount; i++) {
                int pageId = input.readInt();
                if (pageId < 0 || pageId >= nextPageId || !freePageIds.add(pageId)) {
                    throw error(StorageErrorCode.CORRUPT_STORAGE, "open", pageId,
                            "invalid or duplicate free page id");
                }
            }
            if (input.read() != -1) {
                throw error(StorageErrorCode.CORRUPT_STORAGE, "open", null,
                        "allocation metadata has unexpected trailing bytes");
            }
        } catch (EOFException e) {
            throw error(StorageErrorCode.CORRUPT_STORAGE, "open", null,
                    "allocation metadata is incomplete", e);
        }
    }

    private void persistAllocationState() throws StorageException {
        Path temporary = allocationPath.resolveSibling(
                allocationPath.getFileName().toString() + ".tmp");

        try (DataOutputStream output = new DataOutputStream(Files.newOutputStream(
                temporary,
                StandardOpenOption.CREATE,
                StandardOpenOption.TRUNCATE_EXISTING,
                StandardOpenOption.WRITE))) {
            output.writeInt(ALLOCATION_MAGIC);
            output.writeInt(ALLOCATION_VERSION);
            output.writeInt(nextPageId);
            output.writeInt(freePageIds.size());
            for (int pageId : freePageIds) {
                output.writeInt(pageId);
            }
        } catch (IOException e) {
            throw error(StorageErrorCode.IO_ERROR, "persist_allocation", null,
                    "cannot write allocation metadata", e);
        }

        try {
            try {
                Files.move(temporary, allocationPath,
                        StandardCopyOption.REPLACE_EXISTING,
                        StandardCopyOption.ATOMIC_MOVE);
            } catch (AtomicMoveNotSupportedException e) {
                Files.move(temporary, allocationPath,
                        StandardCopyOption.REPLACE_EXISTING);
            }
        } catch (IOException e) {
            throw error(StorageErrorCode.IO_ERROR, "persist_allocation", null,
                    "cannot publish allocation metadata", e);
        }
    }

    private void writeRawPage(int pageId, byte[] data) throws StorageException {
        ByteBuffer buffer = ByteBuffer.wrap(data);
        long offset = pageOffset(pageId);

        try {
            while (buffer.hasRemaining()) {
                int count = channel.write(buffer, offset + buffer.position());
                if (count == 0) {
                    throw error(StorageErrorCode.IO_ERROR, "write_page", pageId,
                            "page write made no progress");
                }
            }
        } catch (IOException e) {
            throw error(StorageErrorCode.IO_ERROR, "write_page", pageId,
                    "cannot write page", e);
        }
    }

    private void ensureAllocated(int pageId, String operation) throws StorageException {
        if (pageId < 0) {
            throw error(StorageErrorCode.INVALID_PAGE_ID, operation, pageId,
                    "page id must be non-negative");
        }
        if (pageId >= nextPageId || freePageIds.contains(pageId)) {
            throw error(StorageErrorCode.PAGE_NOT_ALLOCATED, operation, pageId,
                    "page is not allocated");
        }
    }

    private void ensureOpen(String operation) throws StorageException {
        if (closed) {
            throw error(StorageErrorCode.STORAGE_CLOSED, operation, null,
                    "storage is closed");
        }
    }

    private void closeAfterFailedOpen() {
        if (channel == null) {
            return;
        }
        try {
            channel.close();
        } catch (IOException ignored) {
            // The original open/metadata failure is more useful to the caller.
        }
    }

    private static long pageOffset(int pageId) {
        return Math.multiplyExact((long) pageId, Page.PAGE_SIZE);
    }

    private static StorageException error(
            StorageErrorCode code,
            String operation,
            Integer pageId,
            String message) {
        return new StorageException(code, operation, pageId, message);
    }

    private static StorageException error(
            StorageErrorCode code,
            String operation,
            Integer pageId,
            String message,
            Throwable cause) {
        return new StorageException(code, operation, pageId, message, cause);
    }
}
