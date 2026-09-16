package minidb;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

/** End-to-end contract for SQL -> ExplainPlan JSON -> measured Java execution. */
public final class ExplainAnalyzeEngineTest {
    private ExplainAnalyzeEngineTest() { }

    public static void main(String[] args) throws Exception {
        if (args.length != 1) throw new IllegalArgumentException("expected explain plan path");
        List<DatabaseEngine.ExecutionResult> results = new DatabaseEngine()
            .executeProgramJson(Files.readString(Path.of(args[0])));
        check(results.size() == 13, "every EXPLAIN demo statement must return a result");

        List<String> plainCreate = explainLines(results.get(0));
        check(plainCreate.size() == 1 && plainCreate.get(0).startsWith("CreateTable"),
            "plain CREATE outline mismatch");
        check(!plainCreate.get(0).contains("actual rows="),
            "plain EXPLAIN must not contain runtime metrics");

        List<String> analyzedCreate = explainLines(results.get(1));
        check(analyzedCreate.get(0).contains("actual rows=0") &&
              analyzedCreate.get(0).contains("loops=1"),
              "EXPLAIN ANALYZE CREATE metrics mismatch");
        check(results.get(2).equals(new DatabaseEngine.CommandResult("INSERT", 3)),
            "ANALYZE CREATE did not expose the table to the following INSERT");

        List<String> plainSelect = explainLines(results.get(3));
        check(hasOperator(plainSelect, "Project") && hasOperator(plainSelect, "Sort") &&
              hasOperator(plainSelect, "Filter") && hasOperator(plainSelect, "SeqScan"),
              "plain SELECT outline omitted an operator");
        for (String line : plainSelect)
            check(!line.contains("actual rows="), "plain SELECT unexpectedly ran");

        List<String> analyzedSelect = explainLines(results.get(4));
        assertMetric(analyzedSelect, "Project", 2);
        assertMetric(analyzedSelect, "Filter", 2);
        assertMetric(analyzedSelect, "SeqScan", 3);
        check(analyzedSelect.get(1).startsWith("  Sort") &&
              analyzedSelect.get(2).startsWith("    Filter") &&
              analyzedSelect.get(3).startsWith("      SeqScan"),
              "EXPLAIN tree indentation mismatch");

        explainLines(results.get(5)); // A plain INSERT is displayed but never executed.
        assertSingleValue(results.get(6), 3L, "plain EXPLAIN INSERT changed the table");
        explainLines(results.get(7));
        assertSingleValue(results.get(8), 20L, "plain EXPLAIN UPDATE changed the row");

        List<String> analyzedUpdate = explainLines(results.get(9));
        assertMetric(analyzedUpdate, "Update", 1);
        assertMetric(analyzedUpdate, "SeqScan", 3);
        assertSingleValue(results.get(10), 21L, "EXPLAIN ANALYZE UPDATE was not executed");

        List<String> analyzedDelete = explainLines(results.get(11));
        assertMetric(analyzedDelete, "Delete", 1);
        assertMetric(analyzedDelete, "Filter", 1);
        assertSingleValue(results.get(12), 2L, "EXPLAIN ANALYZE DELETE was not executed");
        System.out.println("ExplainAnalyzeEngineTest passed");
    }

    private static List<String> explainLines(DatabaseEngine.ExecutionResult raw) {
        if (!(raw instanceof DatabaseEngine.QueryResult query))
            throw new AssertionError("EXPLAIN must return a query result");
        check(query.columns().equals(List.of("QUERY PLAN")), "EXPLAIN column name mismatch");
        return query.rows().stream().map(row -> (String) row.get(0)).toList();
    }

    private static boolean hasOperator(List<String> lines, String operator) {
        return lines.stream().anyMatch(line -> line.stripLeading().startsWith(operator));
    }

    private static void assertMetric(List<String> lines, String operator, long rows) {
        String line = lines.stream()
            .filter(value -> value.stripLeading().startsWith(operator))
            .findFirst().orElseThrow(() -> new AssertionError("missing " + operator));
        check(line.contains("actual rows=" + rows + " ") && line.contains("time=") &&
              line.contains("loops=1"), operator + " metric mismatch: " + line);
    }

    private static void assertSingleValue(DatabaseEngine.ExecutionResult raw,
                                          Object expected, String message) {
        if (!(raw instanceof DatabaseEngine.QueryResult query) || query.rows().size() != 1 ||
            query.rows().get(0).size() != 1 || !expected.equals(query.rows().get(0).get(0)))
            throw new AssertionError(message);
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
