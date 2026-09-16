package minidb;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** Checks SQL-to-JSON-to-Java aggregate behavior, including NULL and empty inputs. */
public final class AggregateQueryEngineTest {
    private AggregateQueryEngineTest() { }

    public static void main(String[] args) throws Exception {
        if (args.length != 2)
            throw new IllegalArgumentException("expected aggregate and overflow plan JSON paths");
        List<DatabaseEngine.ExecutionResult> results =
            new DatabaseEngine().executeProgramJson(Files.readString(Path.of(args[0])));

        check(results.size() == 19, "every aggregate script statement must produce one result");
        assertRows(results.get(6), List.of(
            List.of(5L, 4L, 42L, 10.5, "q", 4.5)), "global aggregates");

        List<List<Object>> grouped = new ArrayList<>();
        grouped.add(List.of("a", 2L, 30L, 1.5, "x", "y"));
        grouped.add(Arrays.asList(null, 1L, 7L, 4.5, "q", "q"));
        grouped.add(List.of("b", 2L, 5L, 3.0, "z", "z"));
        assertRows(results.get(7), grouped, "grouped aggregates and aggregate alias ordering");

        List<List<Object>> hiddenGroupOrder = new ArrayList<>();
        hiddenGroupOrder.add(List.of("a", 2L));
        hiddenGroupOrder.add(List.of("b", 1L));
        hiddenGroupOrder.add(Arrays.asList(null, 1L));
        assertRows(results.get(8), hiddenGroupOrder, "hidden GROUP BY ordering and COUNT(column)");

        List<List<Object>> emptyGlobal = new ArrayList<>();
        emptyGlobal.add(Arrays.asList(0L, 0L, null, null, null, null));
        assertRows(results.get(9), emptyGlobal, "empty global aggregate");
        assertRows(results.get(10), List.of(), "empty grouped aggregate");
        assertRows(results.get(11), List.of(List.of(12.0, 3.0, 5L, 20L, false, true)),
            "FLOAT SUM AVG and numeric/BOOL extrema");
        assertRows(results.get(12), List.of(List.of(2L), List.of(2L), List.of(1L)),
            "ORDER BY a non-projected group key");
        List<List<Object>> joined = new ArrayList<>();
        joined.add(List.of("a", 30L));
        joined.add(Arrays.asList(null, 7L));
        joined.add(List.of("b", 5L));
        assertRows(results.get(13), joined, "self JOIN aggregate arguments retain relation identity");
        assertRows(results.get(15), emptyGlobal, "physical empty table global aggregate");
        assertRows(results.get(17), List.of(Arrays.asList(1L, 0L, null, null, null, null)),
            "all-NULL input differs from no rows for COUNT(*)");
        assertRows(results.get(18), List.of(Arrays.asList(null, 1L, null)),
            "all-NULL group");
        try {
            new DatabaseEngine().executeProgramJson(Files.readString(Path.of(args[1])));
            throw new AssertionError("SUM must report INT overflow");
        } catch (EngineException expected) {
            check(expected.code().equals("IntegerOverflow"), "wrong aggregate overflow error");
            check(expected.line() != null && expected.line() == 5L &&
                  expected.column() != null && expected.column() == 8L,
                  "aggregate overflow must point to SUM in SQL");
        }
        System.out.println("AggregateQueryEngineTest passed");
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
