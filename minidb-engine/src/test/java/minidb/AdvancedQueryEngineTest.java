package minidb;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

/**
 * Assertions for JSON generated from advanced-query.sql by the real C++ compiler.
 * The repository integration script supplies the generated plan file as argument 0.
 */
public final class AdvancedQueryEngineTest {
    private AdvancedQueryEngineTest() { }

    public static void main(String[] args) throws Exception {
        if (args.length != 1)
            throw new IllegalArgumentException("expected generated plan JSON path");
        List<DatabaseEngine.ExecutionResult> results =
            new DatabaseEngine().executeProgramJson(Files.readString(Path.of(args[0])));

        check(results.size() == 21, "every statement must produce one result");
        assertRows(results.get(11), List.of(
            List.of(1L, 95.0),
            List.of(2L, 91.0),
            List.of(4L, 91.0)), "JOIN/filter/multi-column sort");

        // List.of rejects null, so build the expected NULL grouping row explicitly.
        List<List<Object>> grouped = new java.util.ArrayList<>();
        grouped.add(java.util.Arrays.asList((Object) null));
        grouped.add(List.of(20L));
        grouped.add(List.of(18L));
        assertRows(results.get(12), grouped, "GROUP BY distinct keys and DESC NULL order");

        assertRows(results.get(13), List.of(
            List.of("Dave"), List.of("Alice"), List.of("Carol"), List.of("Bob")),
            "hidden FLOAT sort key and ASC NULL order");
        assertRows(results.get(14), List.of(
            List.of("Bob"), List.of("Carol"), List.of("Alice"), List.of("Dave")),
            "DESC NULL order");
        assertRows(results.get(15), List.of(
            List.of("Alice"), List.of("Carol"), List.of("Dave")),
            "BOOL predicate and hidden INT sort key");
        DatabaseEngine.QueryResult aliases = assertRows(results.get(20), List.of(
            List.of("Intern", "Developer"),
            List.of("Developer", "CEO"),
            List.of("CEO", "CEO")), "self JOIN with table and ORDER BY aliases");
        check(aliases.columns().equals(List.of("employee_name", "manager_name")),
            "explicit and implicit column aliases must become output names");

        System.out.println("AdvancedQueryEngineTest passed");
    }

    private static DatabaseEngine.QueryResult assertRows(
        DatabaseEngine.ExecutionResult raw, List<List<Object>> expected, String label) {
        if (!(raw instanceof DatabaseEngine.QueryResult query))
            throw new AssertionError(label + ": expected query result");
        check(query.rows().equals(expected),
            label + ": expected " + expected + ", got " + query.rows());
        return query;
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
