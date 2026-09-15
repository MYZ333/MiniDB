package minidb;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

/** End-to-end checks for the syntax delivered by A and implemented by B. */
public final class AExtensionFeaturesEngineTest {
    private AExtensionFeaturesEngineTest() { }

    public static void main(String[] args) throws Exception {
        if (args.length != 3) throw new AssertionError("expected three generated plan paths");
        DatabaseEngine engine = new DatabaseEngine();
        List<DatabaseEngine.ExecutionResult> results =
            engine.executeProgramJson(Files.readString(Path.of(args[0])));
        check(results.size() == 24, "unexpected statement result count");

        command(results, 0, "CREATE", 0);
        command(results, 1, "CREATE", 0); // IF NOT EXISTS is a version-preserving no-op.
        rows(results, 7, List.of("name", "category"), List.of(
            List.of("Alice", "adult"), List.of("Bob", "minor"),
            List.of("Carol", "adult")));
        rows(results, 8, List.of("name"), List.of(List.of("Alice"), List.of("Bob")));
        rows(results, 9, List.of("name"), List.of(List.of("Alice"), List.of("Bob")));
        rows(results, 10, List.of("name", "score"), List.of(List.of("Bob", 80L)));
        rows(results, 11, List.of("name"), List.of(List.of("Alice")));
        rows(results, 12, List.of("name"), List.of(List.of("Alice"), List.of("Bob")));
        rows(results, 13, List.of("id"), List.of(
            List.of(1L), List.of(2L), List.of(3L), List.of(4L)));
        rows(results, 14, List.of("id"), List.of(
            List.of(1L), List.of(2L), List.of(3L), List.of(1L),
            List.of(1L), List.of(2L), List.of(4L)));
        rows(results, 15, List.of("id"), List.of(List.of(1L), List.of(2L)));
        rows(results, 16, List.of("id"), List.of(List.of(3L)));
        rows(results, 17, List.of("name", "enabled"), List.of(
            List.of("Alice", "yes"), List.of("Bob", "no"), List.of("Carol", "yes")));
        command(results, 18, "ALTER", 0);
        command(results, 19, "ALTER", 0);
        rows(results, 20, List.of("name", "alias"), List.of(
            List.of("Alice", "n/a"), List.of("Bob", "n/a"), List.of("Carol", "n/a")));
        command(results, 21, "ALTER", 0);
        command(results, 22, "ALTER", 0);
        rows(results, 23, List.of("name"), List.of(
            List.of("Alice"), List.of("Bob"), List.of("Carol")));
        expectFailure(Path.of(args[1]), "ConstraintViolation");
        expectFailure(Path.of(args[2]), "ScalarSubqueryCardinality");
        System.out.println("AExtensionFeaturesEngineTest passed");
    }

    private static void command(List<DatabaseEngine.ExecutionResult> results, int index,
                                String operation, long affected) {
        DatabaseEngine.CommandResult result = (DatabaseEngine.CommandResult) results.get(index);
        check(result.operation().equals(operation) && result.affectedRows() == affected,
              "unexpected command result at " + index);
    }

    private static void rows(List<DatabaseEngine.ExecutionResult> results, int index,
                             List<String> columns, List<List<Object>> rows) {
        DatabaseEngine.QueryResult result = (DatabaseEngine.QueryResult) results.get(index);
        check(result.columns().equals(columns), "unexpected columns at " + index);
        check(result.rows().equals(rows), "unexpected rows at " + index + ": " + result.rows());
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void expectFailure(Path plan, String code) throws Exception {
        try {
            new DatabaseEngine().executeProgramJson(Files.readString(plan));
            throw new AssertionError("expected engine failure " + code);
        } catch (EngineException error) {
            check(error.code().equals(code),
                  "expected " + code + ", got " + error.code());
        }
    }
}
