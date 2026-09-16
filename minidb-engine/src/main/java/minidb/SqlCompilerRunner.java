package minidb;

import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.nio.file.Files;
import java.time.Duration;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.List;

/** Boundary around the C++ SQL-to-plan program; tests can replace it without a native process. */
interface SqlCompilerRunner {
    CompilerOutput compile(String sql) throws IOException;
    default CompilerOutput compile(String sql, String catalogSnapshot) throws IOException { return compile(sql); }

    record CompilerOutput(int exitCode, String stdout, String stderr) { }
}

final class ProcessSqlCompilerRunner implements SqlCompilerRunner {
    private static final ExecutorService STREAM_READERS = Executors.newCachedThreadPool(task -> {
        Thread thread = new Thread(task, "minidb-compiler-stream");
        thread.setDaemon(true);
        return thread;
    });

    private final Path executable;
    private final Duration timeout;

    ProcessSqlCompilerRunner(Path executable, Duration timeout) {
        this.executable = executable;
        this.timeout = timeout;
    }

    @Override public CompilerOutput compile(String sql) throws IOException {
        return compile(sql, null);
    }

    @Override public CompilerOutput compile(String sql, String catalogSnapshot) throws IOException {
        Path catalogFile = null;
        try {
            List<String> command = new java.util.ArrayList<>(); command.add(executable.toString());
            if (catalogSnapshot != null) { catalogFile = Files.createTempFile("minidb-catalog-", ".txt"); Files.writeString(catalogFile, catalogSnapshot, StandardCharsets.UTF_8); command.add("--catalog-file"); command.add(catalogFile.toString()); }
            Process process = new ProcessBuilder(command).start();
        Future<String> stdout = STREAM_READERS.submit(() -> new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8));
        Future<String> stderr = STREAM_READERS.submit(() -> new String(process.getErrorStream().readAllBytes(), StandardCharsets.UTF_8));
        try (OutputStream input = process.getOutputStream()) { input.write(sql.getBytes(StandardCharsets.UTF_8)); }
        try {
            if (!process.waitFor(timeout.toMillis(), TimeUnit.MILLISECONDS)) {
                process.destroyForcibly();
                process.waitFor();
                throw new IOException("SQL compiler timed out after " + timeout.toSeconds() + " seconds");
            }
            return new CompilerOutput(process.exitValue(), read(stdout), read(stderr));
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            process.destroyForcibly();
            throw new IOException("SQL compiler was interrupted", error);
        }
        } finally { if (catalogFile != null) Files.deleteIfExists(catalogFile); }
    }

    private static String read(Future<String> future) throws IOException {
        try { return future.get(); }
        catch (InterruptedException error) { Thread.currentThread().interrupt(); throw new IOException("interrupted while reading compiler output", error); }
        catch (ExecutionException error) { throw new IOException("failed to read compiler output", error.getCause()); }
    }
}
