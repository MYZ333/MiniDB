package minidb;

import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.time.Duration;
import java.util.Map;

/** Dependency-free HTTP smoke tests; run with assertions enabled. */
public final class WebServerTest {
    private static final String PROGRAM = """
        {"protocolVersion":1,"plans":[
          {"catalogVersion":0,"root":{"type":"CreateTable","tableName":"student","columns":[{"name":"id","type":"INT","primaryKey":true},{"name":"name","type":"VARCHAR"}],"output":[],"carriesRowId":false}},
          {"catalogVersion":1,"root":{"type":"Insert","table":{"id":1,"name":"student","columns":[{"id":1,"name":"id","type":"INT"},{"id":2,"name":"name","type":"VARCHAR"}]},"values":[1,"Alice"],"output":[],"carriesRowId":false}},
          {"catalogVersion":1,"root":{"type":"Insert","table":{"id":1,"name":"student","columns":[{"id":1,"name":"id","type":"INT"},{"id":2,"name":"name","type":"VARCHAR"}]},"values":[2,"Bob"],"output":[],"carriesRowId":false}},
          {"catalogVersion":1,"root":{"type":"Insert","table":{"id":1,"name":"student","columns":[{"id":1,"name":"id","type":"INT"},{"id":2,"name":"name","type":"VARCHAR"}]},"values":[3,null],"output":[],"carriesRowId":false}},
          {"catalogVersion":1,"root":{"type":"Project","columns":[{"tableId":1,"columnId":2,"ordinal":1,"type":"VARCHAR"}],"input":{"type":"SeqScan","table":{"id":1,"name":"student","columns":[{"id":1,"name":"id","type":"INT"},{"id":2,"name":"name","type":"VARCHAR"}]},"output":[],"carriesRowId":false},"output":[{"name":"name","type":"VARCHAR"}],"carriesRowId":false}}
        ]}""";

    public static void main(String[] args) throws Exception {
        SqlCompilerRunner runner = sql -> {
            if (sql.equals("compiler-error")) return new SqlCompilerRunner.CompilerOutput(1, "partial-json", "Compilation failed: unexpected token (line 2, column 5)");
            if (sql.equals("invalid-plan")) return new SqlCompilerRunner.CompilerOutput(0, "{}", "");
            return new SqlCompilerRunner.CompilerOutput(0, PROGRAM, "");
        };
        try (WebServer server = WebServer.start(0, runner, "fake-minisql_plan_json.exe")) {
            HttpClient client = HttpClient.newHttpClient();
            String base = "http://127.0.0.1:" + server.port();
            HttpResponse<String> index = client.send(HttpRequest.newBuilder(URI.create(base + "/")).GET().build(), HttpResponse.BodyHandlers.ofString());
            require(index.statusCode() == 200 && index.body().contains("MiniDB") && index.body().contains("PERSISTENT STORAGE") &&
                !index.body().contains("空内存数据库"), "page should describe persistent storage");
            HttpResponse<String> consoleScript = get(client, base + "/app.js");
            require(consoleScript.statusCode() == 200 && consoleScript.body().contains("验证表目录、主键、UNIQUE、NOT NULL 与 DEFAULT。") &&
                consoleScript.body().contains("预期：") && consoleScript.body().contains("会重建 demo_catalog。"),
                "demo scripts should retain their full purpose, expectation and risk descriptions");
            HttpResponse<String> tableBrowser = get(client, base + "/table-browser.html");
            require(tableBrowser.statusCode() == 200 && tableBrowser.body().contains("TABLE BROWSER"), "table browser page should be served");
            HttpResponse<String> tableBrowserScript = get(client, base + "/table-browser.js");
            require(tableBrowserScript.statusCode() == 200 && tableBrowserScript.body().contains("loadTable"), "table browser script should be served");
            HttpResponse<String> health = client.send(HttpRequest.newBuilder(URI.create(base + "/api/health")).GET().build(), HttpResponse.BodyHandlers.ofString());
            require(health.statusCode() == 200, "health should succeed");
            require(health.body().contains("\"mode\":\"in-memory-test\"") && health.body().contains("\"storage\""),
                "health should expose its storage mode");
            HttpResponse<String> emptyCatalog = get(client, base + "/api/catalog");
            require(emptyCatalog.statusCode() == 200 && emptyCatalog.body().contains("\"tables\":[]"), "catalog should start empty");
            HttpResponse<String> success = post(client, base, "SELECT name FROM student;");
            Map<String, Object> body = object(Json.parse(success.body()));
            require(success.statusCode() == 200 && Boolean.TRUE.equals(body.get("ok")), "execution should succeed");
            require(success.body().contains("Alice") && success.body().contains("\"plan\"") && success.body().contains("\"storage\""),
                "result should contain rows, plan and storage state");
            HttpResponse<String> catalog = get(client, base + "/api/catalog");
            require(catalog.statusCode() == 200 && catalog.body().contains("\"name\":\"student\"") && catalog.body().contains("\"rowCount\":3"),
                "catalog should expose the created table and its row count");
            HttpResponse<String> firstPage = get(client, base + "/api/tables/student?offset=0&limit=1");
            require(firstPage.statusCode() == 200 && firstPage.body().contains("\"columns\":[\"id\",\"name\"]") && firstPage.body().contains("\"primaryKey\":true") && firstPage.body().contains("Alice") &&
                firstPage.body().contains("\"totalRows\":3"), "table browser should return metadata and first data page");
            HttpResponse<String> secondPage = get(client, base + "/api/tables/student?offset=1&limit=1");
            require(secondPage.statusCode() == 200 && secondPage.body().contains("Bob") && secondPage.body().contains("\"offset\":1"),
                "table browser should return later pages");
            HttpResponse<String> nullPage = get(client, base + "/api/tables/student?offset=2&limit=1");
            require(nullPage.statusCode() == 200 && nullPage.body().contains("[3,null]"), "table browser should preserve NULL values");
            HttpResponse<String> invalidPage = get(client, base + "/api/tables/student?limit=101");
            require(invalidPage.statusCode() == 400 && invalidPage.body().contains("InvalidPagination"), "table browser should validate page size");
            HttpResponse<String> missingTable = get(client, base + "/api/tables/missing");
            require(missingTable.statusCode() == 404 && missingTable.body().contains("TableNotFound"), "missing tables should return a clear error");
            HttpResponse<String> compilerError = post(client, base, "compiler-error");
            require(compilerError.statusCode() == 422 && compilerError.body().contains("CompilationFailed") && compilerError.body().contains("\"line\":2"), "compiler diagnostics should retain location");
            HttpResponse<String> protocolError = post(client, base, "invalid-plan");
            require(protocolError.statusCode() == 422 && protocolError.body().contains("\"stage\":\"protocol\"") && protocolError.body().contains("ProtocolError"), "invalid plan should be reported");
        }
        System.out.println("WebServerTest passed.");
    }

    private static HttpResponse<String> post(HttpClient client, String base, String sql) throws Exception {
        String body = Json.stringify(Map.of("sql", sql));
        return client.send(HttpRequest.newBuilder(URI.create(base + "/api/execute")).timeout(Duration.ofSeconds(5)).header("Content-Type", "application/json").POST(HttpRequest.BodyPublishers.ofString(body)).build(), HttpResponse.BodyHandlers.ofString());
    }
    private static HttpResponse<String> get(HttpClient client, String url) throws Exception {
        return client.send(HttpRequest.newBuilder(URI.create(url)).GET().build(), HttpResponse.BodyHandlers.ofString());
    }
    @SuppressWarnings("unchecked") private static Map<String, Object> object(Object value) { return (Map<String, Object>) value; }
    private static void require(boolean condition, String message) { if (!condition) throw new AssertionError(message); }
}
