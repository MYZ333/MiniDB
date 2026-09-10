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
          {"catalogVersion":0,"root":{"type":"CreateTable","tableName":"student","columns":[{"name":"id","type":"INT"},{"name":"name","type":"VARCHAR"}],"output":[],"carriesRowId":false}},
          {"catalogVersion":1,"root":{"type":"Insert","table":{"id":1,"name":"student","columns":[{"id":1,"name":"id","type":"INT"},{"id":2,"name":"name","type":"VARCHAR"}]},"values":[1,"Alice"],"output":[],"carriesRowId":false}},
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
            require(index.statusCode() == 200 && index.body().contains("MiniDB"), "page should be served");
            HttpResponse<String> health = client.send(HttpRequest.newBuilder(URI.create(base + "/api/health")).GET().build(), HttpResponse.BodyHandlers.ofString());
            require(health.statusCode() == 200, "health should succeed");
            HttpResponse<String> success = post(client, base, "SELECT name FROM student;");
            Map<String, Object> body = object(Json.parse(success.body()));
            require(success.statusCode() == 200 && Boolean.TRUE.equals(body.get("ok")), "execution should succeed");
            require(success.body().contains("Alice") && success.body().contains("\"plan\""), "result should contain rows and plan");
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
    @SuppressWarnings("unchecked") private static Map<String, Object> object(Object value) { return (Map<String, Object>) value; }
    private static void require(boolean condition, String message) { if (!condition) throw new AssertionError(message); }
}
