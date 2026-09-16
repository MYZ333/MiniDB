package minidb;

import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;

import java.io.IOException;
import java.io.InputStream;
import java.net.InetSocketAddress;
import java.net.URI;
import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Executors;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import minidb.storage.BufferPool;
import minidb.storage.BufferPoolStats;
import minidb.storage.FileDiskManager;
import minidb.storage.FifoBufferPool;
import minidb.storage.LruBufferPool;
import minidb.storage.StorageException;

/** Local-only HTTP UI for executing one complete MiniDB SQL script at a time. */
public final class WebServer implements AutoCloseable {
    private static final int MAX_REQUEST_BYTES = 1_000_000;
    private static final Pattern LOCATION = Pattern.compile("\\(line (\\d+), column (\\d+)\\)");
    private final HttpServer server;
    private final SqlCompilerRunner compiler;
    private final String compilerPath;
    private final DatabaseEngine engine;
    private final PageRecordStore persistentStore;
    private final Path databasePath;
    private final int bufferFrames;
    private final String bufferPolicy;

    private WebServer(HttpServer server, SqlCompilerRunner compiler, String compilerPath, DatabaseEngine engine,
                      PageRecordStore persistentStore, Path databasePath, int bufferFrames, String bufferPolicy) {
        this.server = server;
        this.compiler = compiler;
        this.compilerPath = compilerPath;
        this.engine = engine;
        this.persistentStore = persistentStore;
        this.databasePath = databasePath;
        this.bufferFrames = bufferFrames;
        this.bufferPolicy = bufferPolicy;
    }

    public static WebServer startFromSystemProperties() throws IOException {
        String configured = System.getProperty("minidb.compiler.path");
        if (configured == null || configured.isBlank())
            throw new IOException("Missing -Dminidb.compiler.path; point it to minisql_plan_json.exe");
        Path executable = Path.of(configured).toAbsolutePath().normalize();
        if (!Files.isRegularFile(executable) || !Files.isExecutable(executable))
            throw new IOException("C++ plan exporter was not found or is not executable: " + executable);
        int port = port(System.getProperty("minidb.web.port", "8080"));
        int frames = Integer.parseInt(System.getProperty("minidb.buffer.frames", "64"));
        Path database = Path.of(System.getProperty("minidb.data.path", "data/minidb.db")).toAbsolutePath().normalize();
        String policy = System.getProperty("minidb.buffer.policy", "LRU").equalsIgnoreCase("FIFO") ? "FIFO" : "LRU";
        try {
            BufferPool pool = policy.equals("FIFO")
                ? new FifoBufferPool(frames, new FileDiskManager(database))
                : new LruBufferPool(frames, new FileDiskManager(database));
            PageRecordStore store = new PageRecordStore(pool);
            return start(port, new ProcessSqlCompilerRunner(executable, Duration.ofSeconds(10)), executable.toString(), new DatabaseEngine(store), store, database, frames, policy);
        } catch (StorageException problem) { throw new IOException("could not open persistent MiniDB storage", problem); }
    }

    static WebServer start(int port, SqlCompilerRunner compiler, String compilerPath) throws IOException {
        HttpServer nativeServer = HttpServer.create(new InetSocketAddress("127.0.0.1", port), 0);
        WebServer app = new WebServer(nativeServer, compiler, compilerPath, new DatabaseEngine(), null, null, 0, "NONE");
        nativeServer.createContext("/", app::handle);
        nativeServer.setExecutor(Executors.newFixedThreadPool(4, task -> {
            Thread thread = new Thread(task, "minidb-web");
            thread.setDaemon(true);
            return thread;
        }));
        nativeServer.start();
        return app;
    }

    private static WebServer start(int port, SqlCompilerRunner compiler, String compilerPath, DatabaseEngine engine,
                                   PageRecordStore store, Path databasePath, int bufferFrames, String bufferPolicy) throws IOException {
        HttpServer nativeServer = HttpServer.create(new InetSocketAddress("127.0.0.1", port), 0);
        WebServer app = new WebServer(nativeServer, compiler, compilerPath, engine, store, databasePath, bufferFrames, bufferPolicy);
        nativeServer.createContext("/", app::handle);
        nativeServer.setExecutor(Executors.newFixedThreadPool(4)); nativeServer.start(); return app;
    }

    public int port() { return server.getAddress().getPort(); }
    @Override public void close() { server.stop(0); if (persistentStore != null) persistentStore.close(); }

    private void handle(HttpExchange exchange) throws IOException {
        try {
            URI request = exchange.getRequestURI();
            String path = request.getPath();
            if (path.equals("/api/health")) { health(exchange); return; }
            if (path.equals("/api/catalog")) { catalog(exchange); return; }
            if (path.startsWith("/api/tables/")) { tableBrowser(exchange, request, path.substring("/api/tables/".length())); return; }
            if (path.equals("/api/execute")) { execute(exchange); return; }
            if (path.equals("/") || path.equals("/index.html")) { resource(exchange, "/web/index.html", "text/html; charset=utf-8"); return; }
            if (path.equals("/table-browser.html")) { resource(exchange, "/web/table-browser.html", "text/html; charset=utf-8"); return; }
            if (path.equals("/app.css")) { resource(exchange, "/web/app.css", "text/css; charset=utf-8"); return; }
            if (path.equals("/app.js")) { resource(exchange, "/web/app.js", "application/javascript; charset=utf-8"); return; }
            if (path.equals("/table-browser.js")) { resource(exchange, "/web/table-browser.js", "application/javascript; charset=utf-8"); return; }
            reply(exchange, 404, error("http", "NotFound", "resource not found", null, null));
        } catch (Exception error) {
            reply(exchange, 500, error("server", "InternalError", error.getMessage() == null ? "unexpected server error" : error.getMessage(), null, null));
        }
    }

    private void health(HttpExchange exchange) throws IOException {
        if (!exchange.getRequestMethod().equals("GET")) { methodNotAllowed(exchange); return; }
        Map<String, Object> response = new LinkedHashMap<>();
        response.put("ok", true); response.put("mode", persistentStore == null ? "in-memory-test" : "persistent"); response.put("compilerPath", compilerPath);
        response.put("compilerAvailable", Files.isRegularFile(Path.of(compilerPath)));
        response.put("storage", storageState());
        reply(exchange, 200, response);
    }

    private void catalog(HttpExchange exchange) throws IOException {
        if (!exchange.getRequestMethod().equals("GET")) { methodNotAllowed(exchange); return; }
        synchronized (engine) {
            List<Object> tables = new ArrayList<>();
            for (DatabaseEngine.TableSummary table : engine.catalogSummaries()) {
                Map<String, Object> item = new LinkedHashMap<>();
                item.put("id", table.id()); item.put("name", table.name()); item.put("columnCount", (long) table.columnCount());
                item.put("indexCount", (long) table.indexCount()); item.put("rowCount", table.rowCount()); tables.add(item);
            }
            Map<String, Object> response = new LinkedHashMap<>();
            response.put("ok", true); response.put("catalogVersion", engine.catalogVersion()); response.put("tables", tables);
            reply(exchange, 200, response);
        }
    }

    private void tableBrowser(HttpExchange exchange, URI request, String encodedName) throws IOException {
        if (!exchange.getRequestMethod().equals("GET")) { methodNotAllowed(exchange); return; }
        String tableName = URLDecoder.decode(encodedName, StandardCharsets.UTF_8);
        if (tableName.isBlank() || tableName.contains("/")) { reply(exchange, 400, error("request", "InvalidTableName", "table name is invalid", null, null)); return; }
        Map<String, String> query = queryParameters(request.getRawQuery());
        int offset;
        int limit;
        try {
            offset = pageParameter(query, "offset", 0, 0, Integer.MAX_VALUE);
            limit = pageParameter(query, "limit", 100, 1, 100);
        } catch (IllegalArgumentException problem) {
            reply(exchange, 400, error("request", "InvalidPagination", problem.getMessage(), null, null)); return;
        }
        synchronized (engine) {
            try {
                DatabaseEngine.TableBrowserSnapshot snapshot = engine.tableBrowserSnapshot(tableName, offset, limit);
                Map<String, Object> table = tableMap(snapshot.table(), snapshot.indexes());
                List<String> columns = snapshot.table().columns().stream().map(DatabaseEngine.ColumnSchema::name).toList();
                Map<String, Object> preview = new LinkedHashMap<>();
                preview.put("columns", columns); preview.put("rows", snapshot.rows()); preview.put("totalRows", snapshot.totalRows());
                preview.put("offset", (long) snapshot.offset()); preview.put("limit", (long) snapshot.limit());
                Map<String, Object> response = new LinkedHashMap<>();
                response.put("ok", true); response.put("catalogVersion", engine.catalogVersion()); response.put("table", table); response.put("preview", preview);
                reply(exchange, 200, response);
            } catch (EngineException problem) {
                int status = problem.code().equals("TableNotFound") ? 404 : 422;
                reply(exchange, status, error("engine", problem.code(), problem.getMessage(), problem.line(), problem.column()));
            }
        }
    }

    private void execute(HttpExchange exchange) throws IOException {
        if (!exchange.getRequestMethod().equals("POST")) { methodNotAllowed(exchange); return; }
        long length = parseLength(exchange.getRequestHeaders().getFirst("Content-Length"));
        if (length > MAX_REQUEST_BYTES) { reply(exchange, 413, error("request", "RequestTooLarge", "SQL script must be at most 1 MB", null, null)); return; }
        Map<String, Object> request;
        try { request = object(Json.parse(new String(exchange.getRequestBody().readAllBytes(), StandardCharsets.UTF_8)), "request"); }
        catch (IllegalArgumentException problem) { reply(exchange, 400, error("request", "InvalidJson", problem.getMessage(), null, null)); return; }
        Object rawSql = request.get("sql");
        if (!(rawSql instanceof String sql) || sql.isBlank()) { reply(exchange, 400, error("request", "MissingSql", "request.sql must be a non-empty string", null, null)); return; }

        synchronized (engine) {
        SqlCompilerRunner.CompilerOutput compiled;
        try { compiled = compiler.compile(sql, engine.catalogSnapshotFileText()); }
        catch (IOException problem) { reply(exchange, 502, error("compiler", "CompilerUnavailable", problem.getMessage(), null, null)); return; }
        if (compiled.exitCode() != 0) { reply(exchange, 422, compilerError(compiled.stderr())); return; }
        Object plan;
        try { plan = Json.parse(compiled.stdout()); }
        catch (IllegalArgumentException problem) { reply(exchange, 422, error("protocol", "InvalidPlanJson", problem.getMessage(), null, null)); return; }
        try {
            List<DatabaseEngine.ExecutionResult> results = engine.executeProgramJson(compiled.stdout());
            Map<String, Object> response = new LinkedHashMap<>();
            response.put("ok", true); response.put("mode", persistentStore == null ? "in-memory-test" : "persistent");
            response.put("storage", storageState()); response.put("results", resultMaps(results)); response.put("plan", plan);
            reply(exchange, 200, response);
        } catch (EngineException problem) {
            String stage = problem.code().equals("ProtocolError") || problem.code().equals("ProtocolMismatch") ? "protocol" : "engine";
            reply(exchange, 422, error(stage, problem.code(), problem.getMessage(), problem.line(), problem.column()));
        } catch (IllegalArgumentException problem) {
            reply(exchange, 422, error("protocol", "InvalidPlan", problem.getMessage(), null, null));
        }
        }
    }

    private static List<Object> resultMaps(List<DatabaseEngine.ExecutionResult> results) {
        List<Object> mapped = new ArrayList<>();
        for (DatabaseEngine.ExecutionResult result : results) {
            Map<String, Object> item = new LinkedHashMap<>();
            if (result instanceof DatabaseEngine.CommandResult command) {
                item.put("kind", "command"); item.put("operation", command.operation()); item.put("affectedRows", command.affectedRows());
            } else {
                DatabaseEngine.QueryResult query = (DatabaseEngine.QueryResult) result;
                item.put("kind", "query"); item.put("columns", query.columns()); item.put("rows", query.rows()); item.put("rowCount", (long) query.rows().size());
            }
            mapped.add(item);
        }
        return mapped;
    }

    private static Map<String, Object> tableMap(DatabaseEngine.TableSchema schema, List<DatabaseEngine.IndexSchema> indexes) {
        Map<String, Object> table = new LinkedHashMap<>();
        table.put("id", schema.id()); table.put("name", schema.name());
        List<Object> columns = new ArrayList<>();
        for (DatabaseEngine.ColumnSchema column : schema.columns()) {
            Map<String, Object> item = new LinkedHashMap<>();
            item.put("id", column.id()); item.put("name", column.name()); item.put("type", column.type());
            item.put("varcharLength", column.varcharLength()); item.put("primaryKey", column.primaryKey());
            item.put("notNull", column.notNull()); item.put("unique", column.unique());
            item.put("hasDefault", column.hasDefault()); item.put("defaultValue", column.defaultValue()); columns.add(item);
        }
        List<Object> constraints = new ArrayList<>();
        for (DatabaseEngine.TableConstraint constraint : schema.constraints())
            constraints.add(Map.of("kind", constraint.kind(), "columns", constraint.columns()));
        List<Object> indexMaps = new ArrayList<>();
        for (DatabaseEngine.IndexSchema index : indexes) {
            Map<String, Object> item = new LinkedHashMap<>();
            item.put("id", index.id()); item.put("name", index.name()); item.put("columnId", index.columnId());
            item.put("keyType", index.keyType()); item.put("unique", index.unique()); indexMaps.add(item);
        }
        table.put("columns", columns); table.put("constraints", constraints); table.put("indexes", indexMaps);
        return table;
    }

    private static Map<String, String> queryParameters(String rawQuery) {
        Map<String, String> values = new LinkedHashMap<>();
        if (rawQuery == null || rawQuery.isBlank()) return values;
        for (String part : rawQuery.split("&")) {
            String[] keyValue = part.split("=", 2);
            String key = URLDecoder.decode(keyValue[0], StandardCharsets.UTF_8);
            String value = keyValue.length == 2 ? URLDecoder.decode(keyValue[1], StandardCharsets.UTF_8) : "";
            values.put(key, value);
        }
        return values;
    }

    private static int pageParameter(Map<String, String> query, String key, int fallback, int min, int max) {
        String raw = query.get(key);
        if (raw == null) return fallback;
        try {
            int value = Integer.parseInt(raw);
            if (value < min || value > max) throw new NumberFormatException();
            return value;
        } catch (NumberFormatException problem) {
            throw new IllegalArgumentException(key + " must be an integer from " + min + " to " + max);
        }
    }

    private Map<String, Object> storageState() {
        Map<String, Object> state = new LinkedHashMap<>();
        boolean persistent = persistentStore != null;
        state.put("persistent", persistent);
        if (!persistent) return state;
        BufferPoolStats stats = persistentStore.bufferPool().stats();
        state.put("databasePath", databasePath.toString());
        state.put("bufferFrames", (long) bufferFrames);
        state.put("bufferPolicy", bufferPolicy);
        state.put("hits", stats.hits());
        state.put("misses", stats.misses());
        state.put("evictions", stats.evictions());
        state.put("flushes", stats.flushes());
        return state;
    }

    private static Map<String, Object> compilerError(String diagnostic) {
        String message = diagnostic == null || diagnostic.isBlank() ? "C++ SQL compiler failed without a diagnostic" : diagnostic.trim();
        Matcher match = LOCATION.matcher(message);
        Long line = null, column = null;
        if (match.find()) { line = Long.parseLong(match.group(1)); column = Long.parseLong(match.group(2)); }
        return error("compiler", "CompilationFailed", message, line, column);
    }

    private static Map<String, Object> error(String stage, String code, String message, Long line, Long column) {
        Map<String, Object> response = new LinkedHashMap<>();
        response.put("ok", false); response.put("stage", stage); response.put("code", code); response.put("message", message);
        if (line != null) response.put("line", line);
        if (column != null) response.put("column", column);
        return response;
    }

    private static void reply(HttpExchange exchange, int status, Map<String, Object> response) throws IOException {
        byte[] body = Json.stringify(response).getBytes(StandardCharsets.UTF_8);
        exchange.getResponseHeaders().set("Content-Type", "application/json; charset=utf-8");
        exchange.getResponseHeaders().set("Cache-Control", "no-store");
        exchange.sendResponseHeaders(status, body.length);
        exchange.getResponseBody().write(body);
        exchange.close();
    }

    private static void methodNotAllowed(HttpExchange exchange) throws IOException {
        exchange.getResponseHeaders().set("Allow", "GET, POST");
        reply(exchange, 405, error("http", "MethodNotAllowed", "method not allowed", null, null));
    }

    private static void resource(HttpExchange exchange, String name, String contentType) throws IOException {
        if (!exchange.getRequestMethod().equals("GET")) { methodNotAllowed(exchange); return; }
        try (InputStream input = WebServer.class.getResourceAsStream(name)) {
            if (input == null) { reply(exchange, 404, error("http", "NotFound", "resource not found", null, null)); return; }
            byte[] body = input.readAllBytes();
            exchange.getResponseHeaders().set("Content-Type", contentType);
            exchange.getResponseHeaders().set("Cache-Control", "no-store");
            exchange.sendResponseHeaders(200, body.length);
            exchange.getResponseBody().write(body);
            exchange.close();
        }
    }

    @SuppressWarnings("unchecked") private static Map<String, Object> object(Object value, String label) {
        if (value instanceof Map<?, ?> map) return (Map<String, Object>) map;
        throw new IllegalArgumentException(label + " must be a JSON object");
    }
    private static long parseLength(String text) { try { return text == null ? 0 : Long.parseLong(text); } catch (NumberFormatException error) { return 0; } }
    private static int port(String text) throws IOException {
        try { int parsed = Integer.parseInt(text); if (parsed < 1 || parsed > 65535) throw new NumberFormatException(); return parsed; }
        catch (NumberFormatException error) { throw new IOException("minidb.web.port must be an integer from 1 to 65535"); }
    }
}
