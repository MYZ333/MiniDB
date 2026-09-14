package minidb;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

/** Executes real optimized JSON and checks both results and visible physical-plan effects. */
public final class OptimizerRulesEngineTest {
    private OptimizerRulesEngineTest() { }

    public static void main(String[] args) throws Exception {
        if (args.length != 1)
            throw new IllegalArgumentException("expected optimizer-rules plan path");
        List<DatabaseEngine.ExecutionResult> results = new DatabaseEngine()
            .executeProgramJson(Files.readString(Path.of(args[0])));
        check(results.size() == 8, "every optimizer demo statement must return a result");

        List<String> joinPlan = explainLines(results.get(4));
        check(operatorCount(joinPlan, "Filter") == 2,
            "both one-relation predicates must run below the join");
        int joinPosition = position(joinPlan, "NestedLoopJoin");
        for (int index = 0; index < joinPlan.size(); index++)
            if (joinPlan.get(index).stripLeading().startsWith("Filter"))
                check(index > joinPosition, "Filter remained above NestedLoopJoin");
        String leftScan = findLine(joinPlan, "SeqScan [opt_student");
        String rightScan = findLine(joinPlan, "SeqScan [opt_score");
        check(leftScan.contains("opt_student.id, opt_student.name, opt_student.age") &&
              !leftScan.contains("opt_student.unused"),
              "left scan column pruning mismatch: " + leftScan);
        check(rightScan.contains("opt_score.student_id, opt_score.value") &&
              !rightScan.contains("opt_score.id") && !rightScan.contains("opt_score.unused"),
              "right scan column pruning mismatch: " + rightScan);
        assertMetric(joinPlan, "Filter", 2);
        assertRows(results.get(5), List.of(
            List.of("Alice", 90L), List.of("Cara", 85L)),
            "optimized join result");

        List<String> countPlan = explainLines(results.get(6));
        String countScan = findLine(countPlan, "SeqScan");
        check(countScan.contains("columns=<none>") && countScan.contains("actual rows=3"),
            "COUNT(*) did not use a zero-column scan: " + countScan);
        assertRows(results.get(7), List.of(List.of(3L)), "COUNT(*) result");
        System.out.println("OptimizerRulesEngineTest passed");
    }

    private static List<String> explainLines(DatabaseEngine.ExecutionResult raw) {
        if (!(raw instanceof DatabaseEngine.QueryResult query))
            throw new AssertionError("EXPLAIN must return a query result");
        return query.rows().stream().map(row -> (String) row.get(0)).toList();
    }

    private static int operatorCount(List<String> lines, String operator) {
        return (int) lines.stream()
            .filter(line -> line.stripLeading().startsWith(operator)).count();
    }

    private static int position(List<String> lines, String operator) {
        for (int index = 0; index < lines.size(); index++)
            if (lines.get(index).stripLeading().startsWith(operator)) return index;
        throw new AssertionError("missing " + operator);
    }

    private static String findLine(List<String> lines, String prefix) {
        return lines.stream().filter(line -> line.stripLeading().startsWith(prefix))
            .findFirst().orElseThrow(() -> new AssertionError("missing " + prefix));
    }

    private static void assertMetric(List<String> lines, String operator, long rows) {
        check(lines.stream().anyMatch(line -> line.stripLeading().startsWith(operator) &&
              line.contains("actual rows=" + rows + " ")),
              "missing " + operator + " rows=" + rows + " metric");
    }

    private static void assertRows(DatabaseEngine.ExecutionResult raw,
                                   List<List<Object>> expected, String label) {
        if (!(raw instanceof DatabaseEngine.QueryResult query) ||
            !query.rows().equals(expected))
            throw new AssertionError(label + ": expected " + expected + ", got " + raw);
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
