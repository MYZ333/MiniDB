package minidb;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** End-to-end assertions for A syntax that is now implemented by B and the engine. */
public final class RemainingFeaturesEngineTest {
    private RemainingFeaturesEngineTest() { }

    public static void main(String[] args) throws Exception {
        if (args.length != 3)
            throw new IllegalArgumentException("expected success, unique and update plan paths");
        verifySuccessfulProgram(Files.readString(Path.of(args[0])));
        expectConstraint(Files.readString(Path.of(args[1])), "duplicate unique value");
        expectConstraint(Files.readString(Path.of(args[2])), "VARCHAR update length");
        System.out.println("RemainingFeaturesEngineTest passed");
    }

    private static void verifySuccessfulProgram(String json) {
        List<DatabaseEngine.ExecutionResult> results =
            new DatabaseEngine().executeProgramJson(json);
        check(results.size() == 14, "every statement must produce a result");
        check(results.get(0).equals(new DatabaseEngine.CommandResult("DROP", 0)),
            "DROP IF EXISTS missing table must be a no-op");
        check(results.get(3).equals(new DatabaseEngine.CommandResult("INSERT", 3)),
            "multi-row INSERT affected count mismatch");

        check(results.get(5).equals(new DatabaseEngine.CommandResult("UPDATE", 1)),
            "UPDATE NULL affected count mismatch");
        assertRows(results.get(6), List.of(
            List.of(10L, false), List.of(9L, false), List.of(8L, true)),
            "computed projection, LIKE, ORDER BY and LIMIT");
        assertRows(results.get(7), List.of(List.of(7L)), "DISTINCT with default values");
        assertRows(results.get(8), List.of(List.of(7L, 3L, 7L)),
            "HAVING and aggregate expression");

        List<List<Object>> left = new ArrayList<>();
        left.add(List.of(1L, "gold"));
        left.add(Arrays.asList(2L, null));
        left.add(Arrays.asList(3L, null));
        assertRows(results.get(9), left, "LEFT JOIN NULL extension");

        List<List<Object>> right = new ArrayList<>();
        right.add(List.of("Alice", "gold"));
        right.add(Arrays.asList(null, "guest"));
        assertRows(results.get(10), right, "RIGHT JOIN NULL extension");

        List<List<Object>> full = new ArrayList<>();
        full.add(List.of("Alice", "gold"));
        full.add(Arrays.asList(null, "guest"));
        full.add(Arrays.asList("Bob", null));
        full.add(Arrays.asList("Cara", null));
        assertRows(results.get(11), full, "FULL JOIN NULL extension");
        check(results.get(12).equals(new DatabaseEngine.CommandResult("DROP", 1)) &&
              results.get(13).equals(new DatabaseEngine.CommandResult("DROP", 1)),
              "DROP affected count mismatch");
    }

    private static void expectConstraint(String json, String label) {
        DatabaseEngine engine = new DatabaseEngine();
        try {
            engine.executeProgramJson(json);
            throw new AssertionError(label + ": expected a constraint failure");
        } catch (EngineException error) {
            check(error.code().equals("ConstraintViolation"),
                label + ": wrong error code " + error.code());
        }
        // The failing statement is the third plan; CREATE and the first two rows remain valid.
        String verification = """
            {"protocolVersion":1,"plans":[{"catalogVersion":1,"root":{
              "type":"Project",
              "columns":[{"tableId":1,"columnId":2,"relationId":1,"ordinal":1,"type":"VARCHAR"}],
              "input":{"type":"SeqScan","table":{"id":1,"name":"constrained"},"relationId":1,
                       "output":[],"carriesRowId":false},
              "output":[{"name":"code","type":"VARCHAR"}],"carriesRowId":false}}]}
            """;
        List<DatabaseEngine.ExecutionResult> after = engine.executeProgramJson(verification);
        assertRows(after.get(0), List.of(List.of("A"), List.of("B")),
            label + " must leave the table unchanged");
    }

    private static void assertRows(DatabaseEngine.ExecutionResult raw,
                                   List<List<Object>> expected, String label) {
        if (!(raw instanceof DatabaseEngine.QueryResult query))
            throw new AssertionError(label + ": expected query result");
        check(query.rows().equals(expected),
            label + ": expected " + expected + ", got " + query.rows());
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
