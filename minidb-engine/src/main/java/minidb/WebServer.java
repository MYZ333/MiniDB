package minidb;

import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;

import java.io.IOException;
import java.io.InputStream;
import java.net.InetSocketAddress;
import java.net.URI;
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

/** Local-only HTTP UI for executing one complete MiniDB SQL script at a time. */
public final class WebServer implements AutoCloseable {
    private static final int MAX_REQUEST_BYTES = 1_000_000;
    private static final Pattern LOCATION = Pattern.compile("\\(line (\\d+), column (\\d+)\\)");
    private final HttpServer server;
    private final SqlCompilerRunner compiler;
    private final String compilerPath;

    private WebServer(HttpServer server, SqlCompilerRunner compiler, String compilerPath) {
        this.server = server;
        this.compiler = compiler;
        this.compilerPath = compilerPath;
    }

    public static WebServer startFromSystemProperties() throws IOException {
        String configured = System.getProperty("minidb.compiler.path");
        if (configured == null || configured.isBlank())
            throw new IOException("Missing -Dminidb.compiler.path; point it to minisql_plan_json.exe");
        Path executable = Path.of(configured).toAbsolutePath().normalize();
        if (!Files.isRegularFile(executable) || !Files.isExecutable(executable))
            throw new IOException("C++ plan exporter was not found or is not executable: " + executable);
        int port = port(System.getProperty("minidb.web.port", "8080"));
        return start(port, new ProcessSqlCompilerRunner(executable, Duration.ofSeconds(10)), executable.toString());
    }

    static WebServer start(int port, SqlCompilerRunner compiler, String compilerPath) throws IOException {
        HttpServer nativeServer = HttpServer.create(new InetSocketAddress("127.0.0.1", port), 0);
        WebServer app = new WebServer(nativeServer, compiler, compilerPath);
        nativeServer.createContext("/", app::handle);
        nativeServer.setExecutor(Executors.newFixedThreadPool(4, task -> {
            Thread thread = new Thread(task, "minidb-web");
            thread.setDaemon(true);
            return thread;
        }));
        nativeServer.start();
        return app;
    }

    public int port() { return server.getAddress().getPort(); }
    @Override public void close() { server.stop(0); }

    private void handle(HttpExchange exchange) throws IOException {
        try {
            URI request = exchange.getRequestURI();
            String path = request.getPath();
            if (path.equals("/api/health")) { health(exchange); return; }
            if (path.equals("/api/execute")) { execute(exchange); return; }
            if (path.equals("/") || path.equals("/index.html")) { resource(exchange, "/web/index.html", "text/html; charset=utf-8"); return; }
            if (path.equals("/app.css")) { resource(exchange, "/web/app.css", "text/css; charset=utf-8"); return; }
            if (path.equals("/app.js")) { resource(exchange, "/web/app.js", "application/javascript; charset=utf-8"); return; }
            reply(exchange, 404, error("http", "NotFound", "resource not found", null, null));
        } catch (Exception error) {
            reply(exchange, 500, error("server", "InternalError", error.getMessage() == null ? "unexpected server error" : error.getMessage(), null, null));
        }
    }

    private void health(HttpExchange exchange) throws IOException {
        if (!exchange.getRequestMethod().equals("GET")) { methodNotAllowed(exchange); return; }
        Map<String, Object> response = new LinkedHashMap<>();
        response.put("ok", true); response.put("mode", "script-replay"); response.put("compilerPath", compilerPath);
        response.put("compilerAvailable", Files.isRegularFile(Path.of(compilerPath)));
        reply(exchange, 200, response);
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

        SqlCompilerRunner.CompilerOutput compiled;
        try { compiled = compiler.compile(sql); }
        catch (IOException problem) { reply(exchange, 502, error("compiler", "CompilerUnavailable", problem.getMessage(), null, null)); return; }
        if (compiled.exitCode() != 0) { reply(exchange, 422, compilerError(compiled.stderr())); return; }

        Object plan;
        try { plan = Json.parse(compiled.stdout()); }
        catch (IllegalArgumentException problem) { reply(exchange, 422, error("protocol", "InvalidPlanJson", problem.getMessage(), null, null)); return; }
        try {
            List<DatabaseEngine.ExecutionResult> results = new DatabaseEngine().executeProgramJson(compiled.stdout());
            Map<String, Object> response = new LinkedHashMap<>();
            response.put("ok", true); response.put("mode", "script-replay"); response.put("results", resultMaps(results)); response.put("plan", plan);
            reply(exchange, 200, response);
        } catch (EngineException problem) {
            String stage = problem.code().equals("ProtocolError") || problem.code().equals("ProtocolMismatch") ? "protocol" : "engine";
            reply(exchange, 422, error(stage, problem.code(), problem.getMessage(), problem.line(), problem.column()));
        } catch (IllegalArgumentException problem) {
            reply(exchange, 422, error("protocol", "InvalidPlan", problem.getMessage(), null, null));
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
