package minidb;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashSet;
import java.util.IdentityHashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Objects;
import java.util.Set;

/** Executes protocolVersion=1 plans emitted by DBcompiler-main/app/plan_json.cpp. */
public final class DatabaseEngine {
    /** Catalog metadata needed to enforce SQL column constraints at the storage boundary. */
    public record ColumnSchema(
        long id, String name, String type, Long varcharLength,
        boolean primaryKey, boolean notNull, boolean unique,
        Object defaultValue, boolean hasDefault) {
        public ColumnSchema(long id, String name, String type) {
            this(id, name, type, null, false, false, false, null, false);
        }
    }
    /** Table-level PRIMARY KEY/UNIQUE keeps the ordered member-column ordinals. */
    public record TableConstraint(String kind, List<Integer> columns) { }
    public record TableSchema(
        long id, String name, List<ColumnSchema> columns,
        List<TableConstraint> constraints) {
        public TableSchema(long id, String name, List<ColumnSchema> columns) {
            this(id, name, columns, List.of());
        }
    }
    public record StoredRow(long id, List<Object> values) { }
    public record CommandResult(String operation, long affectedRows) implements ExecutionResult { }
    public record QueryResult(List<String> columns, List<List<Object>> rows) implements ExecutionResult { }
    public sealed interface ExecutionResult permits CommandResult, QueryResult { }

    /** Identifies a column and relation instance; ordinal remains its source-table position. */
    private record ColumnSlot(
        long tableId, long columnId, long relationId, int ordinal, String type) { }

    /** Intermediate row with column identity and optional per-relation RowId for modification. */
    private record PlanRow(Map<Long, Long> rowIds, List<ColumnSlot> layout, List<Object> values) { }

    /** Materialized aggregate row keeps hidden GROUP BY values for ORDER BY. */
    private record AggregateRow(
        List<Object> groupValues, List<Object> outputValues, List<PlanRow> sourceRows) { }

    /** One runtime sample per concrete JSON plan-node object. Times are inclusive of children. */
    private record ProfileMetric(long rows, long nanos, long loops) { }

    private static final class Profiler {
        private final IdentityHashMap<Map<String, Object>, ProfileMetric> metrics =
            new IdentityHashMap<>();

        void record(Map<String, Object> node, long rows, long nanos) {
            ProfileMetric previous = metrics.get(node);
            metrics.put(node, previous == null
                ? new ProfileMetric(rows, nanos, 1)
                : new ProfileMetric(previous.rows() + rows, previous.nanos() + nanos,
                                    previous.loops() + 1));
        }

        ProfileMetric get(Map<String, Object> node) { return metrics.get(node); }
    }

    private final RecordStore records;
    private final Map<Long, TableSchema> tablesById = new LinkedHashMap<>();
    private final Map<String, TableSchema> tablesByName = new LinkedHashMap<>();
    private long catalogVersion;
    private long nextTableId = 1;
    private Profiler activeProfiler;
    // 关联子查询执行期间保存外层行；列解析找不到本层槽位时从这里读取。
    private PlanRow correlationRow;

    public DatabaseEngine() { this(new InMemoryRecordStore()); }
    public DatabaseEngine(RecordStore records) {
        this.records = Objects.requireNonNull(records);
        records.loadCatalog().ifPresent(catalog -> {
            catalogVersion = catalog.version();
            nextTableId = catalog.nextTableId();
            for (TableSchema table : catalog.tables()) {
                tablesById.put(table.id(), table);
                tablesByName.put(normalize(table.name()), table);
            }
        });
    }

    /** JSON sidecar consumed by minisql_plan_json --catalog-file. */
    public String catalogSnapshotJson() {
        Map<String, Object> root = new LinkedHashMap<>();
        root.put("catalogVersion", catalogVersion);
        root.put("nextTableId", nextTableId);
        List<Object> tables = new ArrayList<>();
        for (TableSchema table : tablesById.values()) {
            Map<String, Object> encoded = new LinkedHashMap<>();
            encoded.put("id", table.id()); encoded.put("name", table.name());
            List<Object> columns = new ArrayList<>();
            for (ColumnSchema column : table.columns()) {
                Map<String, Object> value = new LinkedHashMap<>();
                value.put("id", column.id()); value.put("name", column.name()); value.put("type", column.type());
                value.put("varcharLength", column.varcharLength()); value.put("primaryKey", column.primaryKey());
                value.put("notNull", column.notNull()); value.put("unique", column.unique());
                value.put("defaultValue", column.defaultValue()); value.put("hasDefault", column.hasDefault());
                columns.add(value);
            }
            List<Object> constraints = new ArrayList<>();
            for (TableConstraint constraint : table.constraints())
                constraints.add(Map.of("kind", constraint.kind(), "columns", constraint.columns()));
            encoded.put("columns", columns); encoded.put("constraints", constraints); tables.add(encoded);
        }
        root.put("tables", tables);
        return Json.stringify(root);
    }

    /** Compact line format for the native compiler sidecar, with UTF-8 fields hex encoded. */
    public String catalogSnapshotFileText() {
        StringBuilder text = new StringBuilder("M ").append(catalogVersion).append(' ').append(nextTableId).append('\n');
        for (TableSchema table : tablesById.values()) {
            text.append("T ").append(table.id()).append(' ').append(hex(table.name())).append(' ')
                .append(table.columns().size()).append(' ').append(table.constraints().size()).append('\n');
            for (ColumnSchema column : table.columns()) {
                String kind = "N", value = "";
                if (column.hasDefault()) {
                    Object defaultValue = column.defaultValue();
                    kind = defaultValue instanceof Long ? "L" : defaultValue instanceof Double ? "F" : defaultValue instanceof Boolean ? "B" : "S";
                    value = defaultValue instanceof Boolean truth ? (truth ? "1" : "0") : String.valueOf(defaultValue);
                }
                text.append("C ").append(column.id()).append(' ').append(hex(column.name())).append(' ').append(column.type()).append(' ')
                    .append(column.varcharLength() == null ? -1 : column.varcharLength()).append(' ')
                    .append(column.primaryKey() ? 1 : 0).append(' ').append(column.notNull() ? 1 : 0).append(' ').append(column.unique() ? 1 : 0).append(' ')
                    .append(kind).append(' ').append(hex(value)).append('\n');
            }
            for (TableConstraint constraint : table.constraints()) {
                text.append("K ").append(constraint.kind().equals("PRIMARY_KEY") ? 1 : 0).append(' ').append(constraint.columns().size());
                for (int ordinal : constraint.columns()) text.append(' ').append(ordinal);
                text.append('\n');
            }
        }
        return text.toString();
    }

    private static String hex(String value) { return value.isEmpty() ? "-" : java.util.HexFormat.of().formatHex(value.getBytes(StandardCharsets.UTF_8)); }

    private void persistCatalog() {
        records.persistCatalog(new RecordStore.CatalogState(catalogVersion, nextTableId,
            List.copyOf(tablesById.values())));
    }

    public List<ExecutionResult> executeProgramJson(String json) {
        Map<String, Object> program = map(Json.parse(json), "program");
        if (longValue(program.get("protocolVersion"), "protocolVersion") != 1)
            throw new EngineException("ProtocolMismatch", "unsupported plan protocol");
        List<ExecutionResult> results = new ArrayList<>();
        for (Object plan : list(program.get("plans"), "plans"))
            results.add(executePlan(map(plan, "plan")));
        return results;
    }

    public ExecutionResult executePlan(Map<String, Object> plan) {
        if (longValue(plan.get("catalogVersion"), "catalogVersion") != catalogVersion)
            throw new EngineException("CatalogVersionMismatch",
                "logical plan was compiled against a stale catalog version");
        return executeNode(map(plan.get("root"), "root"));
    }

    private ExecutionResult executeNode(Map<String, Object> node) {
        String type = string(node.get("type"), "node type");
        if (type.equals("Explain")) return explain(node);
        if (activeProfiler == null) return executeNodeRaw(node, type);
        long started = System.nanoTime();
        ExecutionResult result = executeNodeRaw(node, type);
        activeProfiler.record(node, resultRows(result), System.nanoTime() - started);
        return result;
    }

    /** Runs a statement root without adding a second sample around the dispatch itself. */
    private ExecutionResult executeNodeRaw(Map<String, Object> node, String type) {
        return switch (type) {
            case "CreateTable" -> createTable(node);
            case "AlterTable" -> alterTable(node);
            case "DropTable" -> dropTable(node);
            case "Insert" -> insert(node);
            case "Project" -> project(node);
            case "Aggregate" -> aggregate(node);
            case "SetOperation" -> setOperation(node);
            case "Update" -> update(node);
            case "Delete" -> delete(node);
            case "SeqScan", "EmptyResult", "DerivedTable", "NestedLoopJoin", "Filter",
                 "GroupBy", "Sort" ->
                throw new EngineException("InvalidPlan", type + " cannot be an execution root");
            default -> throw new EngineException("InvalidPlan", "unknown plan node: " + type);
        };
    }

    /** EXPLAIN is read-only; ANALYZE executes the same child plan while profiling it. */
    private QueryResult explain(Map<String, Object> node) {
        List<Object> output = list(node.get("output"), "EXPLAIN output");
        Map<String, Object> outputColumn = output.size() == 1
            ? map(output.get(0), "EXPLAIN output column") : Map.of();
        if (output.size() != 1 || !"QUERY PLAN".equals(outputColumn.get("name")) ||
            !"VARCHAR".equals(outputColumn.get("type")) ||
            optionalBoolean(node.get("carriesRowId"), false))
            throw new EngineException("InvalidPlan",
                "EXPLAIN output must be one QUERY PLAN VARCHAR column without RowId");
        Map<String, Object> input = map(node.get("input"), "EXPLAIN input");
        boolean analyze = optionalBoolean(node.get("analyze"), false);
        String rootType = string(input.get("type"), "EXPLAIN root type");
        if (!List.of("CreateTable", "AlterTable", "DropTable", "Insert", "Project",
                     "Aggregate", "SetOperation", "Update", "Delete").contains(rootType))
            throw new EngineException("InvalidPlan",
                rootType + " cannot be an EXPLAIN statement root");
        // Build the outline first so malformed trees fail before ANALYZE can cause a side effect.
        validateExplainTree(input, 0);
        Profiler completed = null;
        if (analyze) {
            if (activeProfiler != null)
                throw new EngineException("InvalidPlan", "nested EXPLAIN ANALYZE is not supported");
            activeProfiler = new Profiler();
            try {
                executeNode(input);
                completed = activeProfiler;
            } finally {
                activeProfiler = null;
            }
        }
        List<String> lines = new ArrayList<>();
        appendExplainLines(input, 0, completed, lines);
        List<List<Object>> rows = new ArrayList<>();
        for (String line : lines) rows.add(List.of(line));
        return new QueryResult(List.of("QUERY PLAN"), List.copyOf(rows));
    }

    private long resultRows(ExecutionResult result) {
        if (result instanceof QueryResult query) return query.rows().size();
        return ((CommandResult) result).affectedRows();
    }

    /** Validates the display tree before ANALYZE starts and therefore before any mutation. */
    private void validateExplainTree(Map<String, Object> node, int depth) {
        if (depth >= 256)
            throw new EngineException("InvalidPlan", "EXPLAIN plan exceeds 256 levels");
        String type = string(node.get("type"), "EXPLAIN node type");
        describeNode(node); // Also validates all attributes used by the presentation layer.
        switch (type) {
            case "NestedLoopJoin", "SetOperation" -> {
                validateExplainTree(map(node.get("left"), "join left input"), depth + 1);
                validateExplainTree(map(node.get("right"), "join right input"), depth + 1);
            }
            case "Filter", "GroupBy", "Aggregate", "Sort", "Project", "DerivedTable",
                 "Update", "Delete" ->
                validateExplainTree(map(node.get("input"), type + " input"), depth + 1);
            case "CreateTable", "AlterTable", "DropTable", "Insert", "SeqScan",
                 "EmptyResult" -> { }
            default -> throw new EngineException("InvalidPlan",
                "unknown EXPLAIN plan node: " + type);
        }
    }

    /** Emits a deterministic pre-order tree so parent and child statistics are easy to compare. */
    private void appendExplainLines(Map<String, Object> node, int depth,
                                    Profiler profiler, List<String> lines) {
        StringBuilder line = new StringBuilder("  ".repeat(depth)).append(describeNode(node));
        if (profiler != null) {
            ProfileMetric metric = profiler.get(node);
            if (metric == null) line.append(" (never executed)");
            else line.append(String.format(Locale.ROOT,
                " (actual rows=%d time=%.3f ms loops=%d)", metric.rows(),
                metric.nanos() / 1_000_000.0, metric.loops()));
        }
        lines.add(line.toString());
        String type = string(node.get("type"), "EXPLAIN node type");
        if (type.equals("NestedLoopJoin") || type.equals("SetOperation")) {
            appendExplainLines(map(node.get("left"), "join left input"), depth + 1,
                               profiler, lines);
            appendExplainLines(map(node.get("right"), "join right input"), depth + 1,
                               profiler, lines);
        } else if (List.of("Filter", "GroupBy", "Aggregate", "Sort", "Project",
                           "DerivedTable", "Update", "Delete").contains(type)) {
            appendExplainLines(map(node.get("input"), type + " input"), depth + 1,
                               profiler, lines);
        }
    }

    private String describeNode(Map<String, Object> node) {
        String type = string(node.get("type"), "EXPLAIN node type");
        return switch (type) {
            case "CreateTable" -> "CreateTable [" +
                string(node.get("tableName"), "tableName") + "]";
            case "AlterTable" -> "AlterTable [" + string(
                map(node.get("table"), "alter table").get("name"), "table name") + "]";
            case "DropTable" -> "DropTable [" + joinStrings(
                list(node.get("tableNames"), "tableNames"), "table name") + "]";
            case "Insert" -> {
                Map<String, Object> table = map(node.get("table"), "insert table");
                List<Object> rows = node.get("rows") instanceof List<?>
                    ? list(node.get("rows"), "insert rows") : List.of();
                yield "Insert [" + string(table.get("name"), "table name") +
                    "; rows=" + (rows.isEmpty() ? 1 : rows.size()) + "]";
            }
            case "SeqScan" -> {
                Map<String, Object> table = map(node.get("table"), "scan table");
                String tableName = string(table.get("name"), "table name");
                String relation = node.get("relationName") instanceof String
                    ? string(node.get("relationName"), "relation name") : tableName;
                yield "SeqScan [" + tableName +
                    (relation.isEmpty() || relation.equals(tableName) ? "" : " AS " + relation) +
                    "; columns=" + describeScanColumns(node) + "]";
            }
            case "EmptyResult" -> {
                emptyLayout(node);
                yield "EmptyResult [columns=" + describeEmptyColumns(node) + "]";
            }
            case "DerivedTable" -> "DerivedTable [" +
                string(node.get("relationName"), "derived relation name") + "]";
            case "NestedLoopJoin" -> "NestedLoopJoin [" +
                (node.get("joinType") == null ? "INNER" :
                    string(node.get("joinType"), "join type")) + "; " +
                describeExpression(map(node.get("predicate"), "join predicate"), 0) + "]";
            case "Filter" -> "Filter [" +
                describeExpression(map(node.get("predicate"), "filter predicate"), 0) + "]";
            case "GroupBy" -> "GroupBy [" + describeReferences(
                list(node.get("keys"), "group keys"), "group key") + "]";
            case "Aggregate" -> "Aggregate [group=" + describeReferences(
                list(node.get("groupKeys"), "group keys"), "group key") +
                "; output=" + describeOutput(node) + aggregateModifiers(node) + "]";
            case "Sort" -> "Sort [" + describeSortItems(
                list(node.get("items"), "sort items")) + "]";
            case "Project" -> "Project [" + describeOutput(node) +
                queryModifiers(node) + "]";
            case "SetOperation" -> "SetOperation [" +
                string(node.get("operation"), "set operation") +
                (optionalBoolean(node.get("all"), false) ? " ALL" : "") + "]";
            case "Update" -> "Update [" + string(
                map(node.get("table"), "update table").get("name"), "table name") +
                "; assignments=" + list(node.get("assignments"), "assignments").size() + "]";
            case "Delete" -> "Delete [" + string(
                map(node.get("table"), "delete table").get("name"), "table name") + "]";
            default -> throw new EngineException("InvalidPlan",
                "unknown EXPLAIN plan node: " + type);
        };
    }

    private String describeOutput(Map<String, Object> node) {
        List<String> names = new ArrayList<>();
        for (Object raw : list(node.get("output"), "output"))
            names.add(string(map(raw, "output column").get("name"), "output name"));
        return String.join(", ", names);
    }

    private String queryModifiers(Map<String, Object> node) {
        StringBuilder result = new StringBuilder();
        if (optionalBoolean(node.get("distinct"), false)) result.append("; DISTINCT");
        Long limit = nodeLong(node.get("limit"));
        if (limit != null) result.append("; limit=").append(limit);
        long offset = optionalLong(node.get("offset"), 0);
        if (offset != 0) result.append("; offset=").append(offset);
        return result.toString();
    }

    private String aggregateModifiers(Map<String, Object> node) {
        StringBuilder result = new StringBuilder(queryModifiers(node));
        if (node.get("having") instanceof Map<?, ?>)
            result.append("; having=").append(describeExpression(
                map(node.get("having"), "HAVING"), 0));
        return result.toString();
    }

    private String describeSortItems(List<Object> values) {
        List<String> items = new ArrayList<>();
        for (Object raw : values) {
            Map<String, Object> item = map(raw, "sort item");
            String kind = item.get("kind") == null
                ? "column" : string(item.get("kind"), "sort kind");
            String key = kind.equals("column")
                ? describeColumn(map(item.get("column"), "sort column"))
                : describeExpression(map(item.get("expression"), "sort expression"), 0);
            items.add(key + " " + string(item.get("direction"), "sort direction"));
        }
        return String.join(", ", items);
    }

    private String describeReferences(List<Object> values, String label) {
        List<String> references = new ArrayList<>();
        for (Object raw : values) references.add(describeColumn(map(raw, label)));
        return references.isEmpty() ? "<all>" : String.join(", ", references);
    }

    /** Shows the physical scan width so EXPLAIN can demonstrate column pruning. */
    private String describeScanColumns(Map<String, Object> node) {
        if (node.get("columns") == null) return "<all>";
        List<Object> columns = list(node.get("columns"), "scan columns");
        if (columns.isEmpty()) return "<none>";
        List<String> names = new ArrayList<>();
        for (Object raw : columns)
            names.add(describeColumn(map(raw, "scan column")));
        return String.join(", ", names);
    }

    private String describeEmptyColumns(Map<String, Object> node) {
        List<Object> columns = list(node.get("columns"), "empty-result columns");
        if (columns.isEmpty()) return "<none>";
        List<String> names = new ArrayList<>();
        for (Object raw : columns)
            names.add(describeColumn(map(raw, "empty-result column")));
        return String.join(", ", names);
    }

    private String describeColumn(Map<String, Object> reference) {
        long tableId = longValue(reference.get("tableId"), "table id");
        long columnId = longValue(reference.get("columnId"), "column id");
        TableSchema table = tablesById.get(tableId);
        if (table != null) {
            for (ColumnSchema column : table.columns())
                if (column.id() == columnId) return table.name() + "." + column.name();
        }
        return "table#" + tableId + ".column#" + columnId;
    }

    private String describeExpression(Map<String, Object> expression, int depth) {
        if (depth >= 256)
            throw new EngineException("InvalidPlan", "EXPLAIN expression exceeds 256 levels");
        String kind = string(expression.get("kind"), "expression kind");
        return switch (kind) {
            case "column" -> describeColumn(map(expression.get("column"), "column"));
            case "literal" -> displayLiteral(expression.get("value"));
            case "aggregate" -> string(expression.get("function"), "aggregate function") +
                "(" + (expression.get("argument") == null ? "*" : describeColumn(
                    map(expression.get("argument"), "aggregate argument"))) + ")";
            case "unary" -> describeUnary(expression, depth);
            case "binary" -> "(" + describeExpression(
                map(expression.get("left"), "left expression"), depth + 1) + " " +
                displayBinary(string(expression.get("op"), "binary operator")) + " " +
                describeExpression(map(expression.get("right"), "right expression"),
                                   depth + 1) + ")";
            case "case" -> "CASE ... END";
            case "inSubquery" -> "(" + describeExpression(
                map(expression.get("value"), "IN value"), depth + 1) +
                (optionalBoolean(expression.get("negated"), false)
                    ? " NOT IN (SUBQUERY))" : " IN (SUBQUERY))");
            case "existsSubquery" -> optionalBoolean(expression.get("negated"), false)
                ? "NOT EXISTS (SUBQUERY)" : "EXISTS (SUBQUERY)";
            case "scalarSubquery" -> "(SCALAR SUBQUERY)";
            default -> throw new EngineException("InvalidPlan",
                "unknown expression kind in EXPLAIN: " + kind);
        };
    }

    private String describeUnary(Map<String, Object> expression, int depth) {
        String op = string(expression.get("op"), "unary operator");
        String operand = describeExpression(map(expression.get("operand"), "unary operand"),
                                            depth + 1);
        return switch (op) {
            case "Negate" -> "(-" + operand + ")";
            case "Not" -> "(NOT " + operand + ")";
            case "IsNull" -> "(" + operand + " IS NULL)";
            case "IsNotNull" -> "(" + operand + " IS NOT NULL)";
            default -> throw new EngineException("InvalidPlan",
                "unknown unary operator in EXPLAIN: " + op);
        };
    }

    private String displayBinary(String op) {
        return switch (op) {
            case "Add" -> "+"; case "Subtract" -> "-";
            case "Multiply" -> "*"; case "Divide" -> "/";
            case "Equal" -> "="; case "NotEqual" -> "!=";
            case "Less" -> "<"; case "LessEqual" -> "<=";
            case "Greater" -> ">"; case "GreaterEqual" -> ">=";
            case "And" -> "AND"; case "Or" -> "OR"; case "Like" -> "LIKE";
            default -> throw new EngineException("InvalidPlan",
                "unknown binary operator in EXPLAIN: " + op);
        };
    }

    private String displayLiteral(Object value) {
        if (value == null) return "NULL";
        if (value instanceof String text) return "'" + text.replace("'", "''") + "'";
        if (value instanceof Boolean truth) return truth ? "TRUE" : "FALSE";
        return String.valueOf(value);
    }

    private String joinStrings(List<Object> values, String label) {
        List<String> result = new ArrayList<>();
        for (Object value : values) result.add(string(value, label));
        return String.join(", ", result);
    }

    private CommandResult createTable(Map<String, Object> node) {
        String name = normalize(string(node.get("tableName"), "tableName"));
        if (tablesByName.containsKey(name) && optionalBoolean(node.get("ifNotExists"), false))
            return new CommandResult("CREATE", 0);
        if (tablesByName.containsKey(name))
            throw new EngineException("TableAlreadyExists", "table already exists: " + name);
        List<ColumnSchema> columns = new ArrayList<>();
        boolean hasPrimaryKey = false;
        for (Object value : list(node.get("columns"), "columns")) {
            Map<String, Object> column = map(value, "column");
            String columnName = normalize(string(column.get("name"), "column name"));
            String type = string(column.get("type"), "column type");
            if (!(type.equals("INT") || type.equals("VARCHAR")
                || type.equals("BOOL") || type.equals("FLOAT")))
                throw new EngineException("UnsupportedType", "unsupported column type: " + type);
            if (columns.stream().anyMatch(existing -> existing.name().equals(columnName)))
                throw new EngineException("DuplicateColumn", "duplicate column: " + columnName);
            Long varcharLength = nodeLong(column.get("varcharLength"));
            if (varcharLength != null && (!type.equals("VARCHAR") || varcharLength <= 0))
                throw new EngineException("UnsupportedType", "VARCHAR length must be positive");
            boolean primaryKey = optionalBoolean(column.get("primaryKey"), false);
            if (primaryKey && hasPrimaryKey)
                throw new EngineException("InvalidPlan", "table may contain only one PRIMARY KEY column");
            hasPrimaryKey |= primaryKey;
            boolean notNull = optionalBoolean(column.get("notNull"), false) || primaryKey;
            boolean unique = optionalBoolean(column.get("unique"), false) || primaryKey;
            boolean hasDefault = optionalBoolean(column.get("hasDefault"), false);
            Object defaultValue = hasDefault
                ? coerceValue(column.get("defaultValue"), type, column) : null;
            ColumnSchema schema = new ColumnSchema(columns.size() + 1L, columnName, type,
                varcharLength, primaryKey, notNull, unique, defaultValue, hasDefault);
            validateColumnValue(schema, defaultValue, hasDefault, column);
            columns.add(schema);
        }
        if (columns.isEmpty())
            throw new EngineException("EmptyColumnList", "table must contain at least one column");
        List<TableConstraint> constraints = new ArrayList<>();
        List<Object> encodedConstraints = node.get("tableConstraints") instanceof List<?>
            ? list(node.get("tableConstraints"), "table constraints") : List.of();
        for (Object raw : encodedConstraints) {
            Map<String, Object> encoded = map(raw, "table constraint");
            String kind = string(encoded.get("kind"), "constraint kind");
            if (!kind.equals("PRIMARY_KEY") && !kind.equals("UNIQUE"))
                throw new EngineException("InvalidPlan", "unknown table constraint: " + kind);
            if (kind.equals("PRIMARY_KEY") && hasPrimaryKey)
                throw new EngineException("InvalidPlan", "table may contain only one PRIMARY KEY");
            List<Integer> members = new ArrayList<>();
            Set<Integer> uniqueMembers = new HashSet<>();
            for (Object value : list(encoded.get("columns"), "constraint columns")) {
                int ordinal = Math.toIntExact(longValue(value, "constraint column ordinal"));
                if (ordinal < 0 || ordinal >= columns.size() || !uniqueMembers.add(ordinal))
                    throw new EngineException("InvalidPlan", "invalid table constraint column");
                members.add(ordinal);
            }
            if (members.isEmpty())
                throw new EngineException("InvalidPlan", "table constraint requires columns");
            if (kind.equals("PRIMARY_KEY")) {
                hasPrimaryKey = true;
                // 复合主键保证每个成员非空，但成员本身不需要单列唯一。
                for (int ordinal : members) {
                    ColumnSchema old = columns.get(ordinal);
                    columns.set(ordinal, new ColumnSchema(old.id(), old.name(), old.type(),
                        old.varcharLength(), old.primaryKey(), true, old.unique(),
                        old.defaultValue(), old.hasDefault()));
                }
            }
            constraints.add(new TableConstraint(kind, List.copyOf(members)));
        }
        TableSchema schema = new TableSchema(nextTableId++, name, List.copyOf(columns),
                                             List.copyOf(constraints));
        records.createTable(schema);
        tablesById.put(schema.id(), schema);
        tablesByName.put(name, schema);
        catalogVersion++;
        persistCatalog();
        return new CommandResult("CREATE", 0);
    }

    /** Applies ALTER atomically from the engine's point of view and preserves table/row IDs. */
    private CommandResult alterTable(Map<String, Object> node) {
        TableSchema table = resolveTable(map(node.get("table"), "alter table"));
        Map<String, Object> action = map(node.get("action"), "alter action");
        String actionType = string(action.get("type"), "alter action type");
        List<ColumnSchema> columns = new ArrayList<>(table.columns());
        List<TableConstraint> constraints = new ArrayList<>(table.constraints());
        TableSchema changed;

        if (actionType.equals("AddColumn")) {
            Map<String, Object> encoded = map(action.get("column"), "added column");
            String name = normalize(string(encoded.get("name"), "column name"));
            if (columns.stream().anyMatch(column -> column.name().equals(name)))
                throw new EngineException("DuplicateColumn", "duplicate column: " + name);
            String type = string(encoded.get("type"), "column type");
            if (!List.of("INT", "VARCHAR", "BOOL", "FLOAT").contains(type))
                throw new EngineException("UnsupportedType", "unsupported column type: " + type);
            Long varcharLength = nodeLong(encoded.get("varcharLength"));
            if (varcharLength != null && (!type.equals("VARCHAR") || varcharLength <= 0))
                throw new EngineException("UnsupportedType", "VARCHAR length must be positive");
            boolean primaryKey = optionalBoolean(encoded.get("primaryKey"), false);
            boolean alreadyPrimary = columns.stream().anyMatch(ColumnSchema::primaryKey) ||
                constraints.stream().anyMatch(value -> value.kind().equals("PRIMARY_KEY"));
            if (primaryKey && alreadyPrimary)
                throw new EngineException("InvalidPlan", "table may contain only one PRIMARY KEY");
            boolean notNull = optionalBoolean(encoded.get("notNull"), false) || primaryKey;
            boolean unique = optionalBoolean(encoded.get("unique"), false) || primaryKey;
            boolean hasDefault = optionalBoolean(encoded.get("hasDefault"), false);
            Object defaultValue = hasDefault
                ? coerceValue(encoded.get("defaultValue"), type, encoded) : null;
            long nextColumnId = columns.stream().mapToLong(ColumnSchema::id).max().orElse(0L) + 1;
            ColumnSchema added = new ColumnSchema(nextColumnId, name, type, varcharLength,
                primaryKey, notNull, unique, defaultValue, hasDefault);
            validateColumnValue(added, defaultValue, hasDefault, encoded);
            columns.add(added);
            changed = new TableSchema(table.id(), table.name(), List.copyOf(columns),
                                      List.copyOf(constraints));
            List<StoredRow> oldRows = records.scan(table.id());
            List<List<Object>> newRows = new ArrayList<>();
            for (StoredRow row : oldRows) {
                List<Object> values = new ArrayList<>(row.values());
                values.add(hasDefault ? defaultValue : null);
                newRows.add(values);
            }
            validateFinalRows(changed, newRows);
            for (int i = 0; i < oldRows.size(); i++)
                records.replace(table.id(), oldRows.get(i).id(), newRows.get(i));
        } else if (actionType.equals("DropColumn")) {
            int ordinal = Math.toIntExact(longValue(action.get("ordinal"), "column ordinal"));
            if (ordinal < 0 || ordinal >= columns.size())
                throw new EngineException("InvalidPlan", "column ordinal is out of range");
            if (columns.size() == 1)
                throw new EngineException("EmptyColumnList", "cannot drop the last table column");
            for (TableConstraint constraint : constraints)
                if (constraint.columns().contains(ordinal))
                    throw new EngineException("ConstraintViolation",
                        "cannot drop a column used by a table constraint");
            columns.remove(ordinal);
            List<TableConstraint> adjusted = new ArrayList<>();
            for (TableConstraint constraint : constraints) {
                List<Integer> members = new ArrayList<>();
                for (int member : constraint.columns())
                    members.add(member > ordinal ? member - 1 : member);
                adjusted.add(new TableConstraint(constraint.kind(), List.copyOf(members)));
            }
            constraints = adjusted;
            changed = new TableSchema(table.id(), table.name(), List.copyOf(columns),
                                      List.copyOf(constraints));
            for (StoredRow row : records.scan(table.id())) {
                List<Object> values = new ArrayList<>(row.values());
                values.remove(ordinal);
                records.replace(table.id(), row.id(), values);
            }
        } else if (actionType.equals("RenameTable")) {
            String newName = normalize(string(action.get("newName"), "new table name"));
            if (tablesByName.containsKey(newName))
                throw new EngineException("TableAlreadyExists", "table already exists: " + newName);
            changed = new TableSchema(table.id(), newName, table.columns(), table.constraints());
            tablesByName.remove(table.name());
        } else if (actionType.equals("RenameColumn")) {
            int ordinal = Math.toIntExact(longValue(action.get("ordinal"), "column ordinal"));
            String newName = normalize(string(action.get("newName"), "new column name"));
            if (ordinal < 0 || ordinal >= columns.size())
                throw new EngineException("InvalidPlan", "column ordinal is out of range");
            if (columns.stream().anyMatch(column -> column.name().equals(newName)))
                throw new EngineException("DuplicateColumn", "duplicate column: " + newName);
            ColumnSchema old = columns.get(ordinal);
            columns.set(ordinal, new ColumnSchema(old.id(), newName, old.type(),
                old.varcharLength(), old.primaryKey(), old.notNull(), old.unique(),
                old.defaultValue(), old.hasDefault()));
            changed = new TableSchema(table.id(), table.name(), List.copyOf(columns),
                                      List.copyOf(constraints));
        } else {
            throw new EngineException("InvalidPlan", "unknown ALTER action: " + actionType);
        }
        tablesById.put(changed.id(), changed);
        tablesByName.put(changed.name(), changed);
        catalogVersion++;
        persistCatalog();
        return new CommandResult("ALTER", 0);
    }

    /** DROP mutates catalog and storage together; IF EXISTS with no match is a no-op. */
    private CommandResult dropTable(Map<String, Object> node) {
        boolean ifExists = optionalBoolean(node.get("ifExists"), false);
        List<String> names = new ArrayList<>();
        Set<String> seen = new HashSet<>();
        for (Object raw : list(node.get("tableNames"), "tableNames")) {
            String name = normalize(string(raw, "table name"));
            if (!seen.add(name))
                throw new EngineException("DuplicateTable", "duplicate table: " + name);
            if (!ifExists && !tablesByName.containsKey(name))
                throw new EngineException("TableNotFound", "table does not exist: " + name);
            names.add(name);
        }
        if (names.isEmpty()) throw new EngineException("InvalidPlan", "DROP requires table names");
        long removed = 0;
        for (String name : names) {
            TableSchema table = tablesByName.remove(name);
            if (table == null) continue;
            records.dropTable(table.id());
            tablesById.remove(table.id());
            removed++;
        }
        if (removed > 0) {
            catalogVersion++;
            persistCatalog();
        }
        return new CommandResult("DROP", removed);
    }

    private CommandResult insert(Map<String, Object> node) {
        TableSchema table = resolveTable(map(node.get("table"), "table"));
        List<Object> encodedRows = node.get("rows") instanceof List<?>
            ? list(node.get("rows"), "rows") : List.of(list(node.get("values"), "values"));
        if (encodedRows.isEmpty()) throw new EngineException("InvalidPlan", "INSERT requires rows");
        List<List<Object>> rows = new ArrayList<>();
        for (Object raw : encodedRows)
            rows.add(normalizeRow(table, list(raw, "insert row"), node));
        List<List<Object>> finalRows = new ArrayList<>();
        for (StoredRow present : records.scan(table.id()))
            finalRows.add(new ArrayList<>(present.values()));
        finalRows.addAll(rows);
        validateFinalRows(table, finalRows);
        for (List<Object> row : rows) records.insert(table.id(), row);
        return new CommandResult("INSERT", rows.size());
    }

    private QueryResult project(Map<String, Object> node) {
        List<PlanRow> input = readInput(map(node.get("input"), "Project input"));
        List<String> names = new ArrayList<>();
        for (Object output : list(node.get("output"), "output"))
            names.add(string(map(output, "output column").get("name"), "output name"));
        List<Map<String, Object>> references =
            objectList(node.get("columns"), "project columns", "column ref");
        List<Map<String, Object>> expressions = node.get("expressions") instanceof List<?>
            ? objectList(node.get("expressions"), "project expressions", "expression")
            : List.of();
        int selectedCount = expressions.isEmpty() ? references.size() : expressions.size();
        if (names.size() != selectedCount)
            throw new EngineException("InvalidPlan", "Project output does not match selected columns");
        List<List<Object>> rows = new ArrayList<>();
        for (PlanRow row : input) {
            List<Object> selected = new ArrayList<>();
            if (expressions.isEmpty()) {
                for (Map<String, Object> reference : references)
                    selected.add(columnValue(reference, row));
            } else {
                for (Map<String, Object> expression : expressions)
                    selected.add(evaluate(expression, row));
            }
            rows.add(selected);
        }
        if (optionalBoolean(node.get("distinct"), false))
            rows = distinctRows(rows);
        rows = window(rows, node);
        return new QueryResult(List.copyOf(names), List.copyOf(rows));
    }

    /** Forms groups, evaluates aggregate states, then sorts the finished result rows. */
    private QueryResult aggregate(Map<String, Object> node) {
        List<Map<String, Object>> groupKeys =
            objectList(node.get("groupKeys"), "aggregate group keys", "group key");
        List<Map<String, Object>> items =
            objectList(node.get("items"), "aggregate items", "aggregate item");
        validateAggregateItems(items, groupKeys);
        List<PlanRow> input = readInput(map(node.get("input"), "Aggregate input"));
        Map<List<Object>, List<PlanRow>> groups = new LinkedHashMap<>();
        // SQL 全表聚合即使输入为空也产生一行；分组聚合的空输入没有分组。
        if (groupKeys.isEmpty()) groups.put(List.of(), new ArrayList<>());
        for (PlanRow row : input) {
            List<Object> key = new ArrayList<>();
            for (Map<String, Object> reference : groupKeys)
                key.add(columnValue(reference, row));
            groups.computeIfAbsent(key, ignored -> new ArrayList<>()).add(row);
        }

        Map<String, Object> having = node.get("having") instanceof Map<?, ?>
            ? map(node.get("having"), "HAVING") : null;
        List<AggregateRow> aggregateRows = new ArrayList<>();
        for (Map.Entry<List<Object>, List<PlanRow>> group : groups.entrySet()) {
            if (having != null && !predicateTrue(
                    evaluateAggregate(having, group.getValue()), having))
                continue;
            List<Object> values = new ArrayList<>();
            for (Map<String, Object> item : items) {
                String kind = string(item.get("kind"), "aggregate item kind");
                if (kind.equals("column")) {
                    if (group.getValue().isEmpty())
                        throw new EngineException("InvalidPlan",
                            "global empty aggregate cannot output a source column");
                    values.add(columnValue(map(item.get("column"), "aggregate column"),
                                           group.getValue().get(0)));
                } else if (kind.equals("aggregate")) {
                    values.add(aggregateValue(item, group.getValue()));
                } else if (kind.equals("expression")) {
                    values.add(evaluateAggregate(
                        map(item.get("expression"), "aggregate expression"), group.getValue()));
                } else {
                    throw new EngineException("InvalidPlan", "unknown aggregate item kind: " + kind);
                }
            }
            // ArrayList allows SQL NULL elements; List.copyOf deliberately rejects them.
            aggregateRows.add(new AggregateRow(
                new ArrayList<>(group.getKey()), values, group.getValue()));
        }

        List<Map<String, Object>> order =
            objectList(node.get("orderBy"), "aggregate order", "aggregate order item");
        // Validate even for zero/one result rows, when sorting never invokes its comparator.
        for (Map<String, Object> item : order) {
            String direction = string(item.get("direction"), "aggregate order direction");
            if (!direction.equals("ASC") && !direction.equals("DESC"))
                throw new EngineException("InvalidPlan", "unknown aggregate sort direction");
            String kind = string(item.get("kind"), "aggregate order kind");
            if (kind.equals("output")) {
                long ordinal = longValue(item.get("ordinal"), "output ordinal");
                if (ordinal < 0 || ordinal >= items.size())
                    throw new EngineException("InvalidPlan", "aggregate output ordinal is out of range");
            } else if (kind.equals("group")) {
                referenceIndex(map(item.get("column"), "order group column"), groupKeys);
            } else if (kind.equals("expression")) {
                map(item.get("expression"), "order expression");
            } else throw new EngineException("InvalidPlan", "unknown aggregate order kind");
        }
        aggregateRows.sort((left, right) -> compareAggregateRows(left, right, order, groupKeys, items));
        if (optionalBoolean(node.get("distinct"), false)) {
            Map<List<Object>, AggregateRow> unique = new LinkedHashMap<>();
            for (AggregateRow row : aggregateRows)
                unique.putIfAbsent(new ArrayList<>(row.outputValues()), row);
            aggregateRows = new ArrayList<>(unique.values());
        }
        aggregateRows = windowAggregateRows(aggregateRows, node);
        List<String> names = new ArrayList<>();
        for (Object output : list(node.get("output"), "output"))
            names.add(string(map(output, "output column").get("name"), "output name"));
        if (names.size() != items.size())
            throw new EngineException("InvalidPlan", "Aggregate output does not match items");
        List<List<Object>> rows = new ArrayList<>();
        for (AggregateRow row : aggregateRows) rows.add(row.outputValues());
        return new QueryResult(List.copyOf(names), List.copyOf(rows));
    }

    /** Check the aggregation contract before inspecting rows, including empty input. */
    private void validateAggregateItems(List<Map<String, Object>> items,
                                        List<Map<String, Object>> groupKeys) {
        if (items.isEmpty())
            throw new EngineException("InvalidPlan", "Aggregate requires output items");
        for (Map<String, Object> item : items) {
            String kind = string(item.get("kind"), "aggregate item kind");
            if (kind.equals("column")) {
                referenceIndex(map(item.get("column"), "aggregate column"), groupKeys);
                continue;
            }
            if (!kind.equals("aggregate")) {
                if (kind.equals("expression")) {
                    map(item.get("expression"), "aggregate expression");
                    continue;
                } else throw error("InvalidPlan", "unknown aggregate item kind", item);
            }
            String function = string(item.get("function"), "aggregate function");
            if (!List.of("COUNT", "SUM", "AVG", "MIN", "MAX").contains(function))
                throw error("InvalidPlan", "unknown aggregate function", item);
            if (item.get("argument") == null && !function.equals("COUNT"))
                throw error("InvalidPlan", "only COUNT accepts '*'", item);
            String argumentType = item.get("argument") == null ? "INT" :
                string(map(item.get("argument"), "aggregate argument").get("type"), "argument type");
            if ((function.equals("SUM") || function.equals("AVG"))
                && !argumentType.equals("INT") && !argumentType.equals("FLOAT"))
                throw error("InvalidPlan", "numeric aggregate requires INT or FLOAT", item);
            String expected = function.equals("COUNT") ? "INT" :
                function.equals("AVG") ? "FLOAT" : argumentType;
            if (!expected.equals(string(item.get("type"), "aggregate type")))
                throw error("InvalidPlan", "aggregate result type is inconsistent", item);
        }
    }

    /** NULL inputs are ignored. COUNT returns zero; every other empty aggregate returns NULL. */
    private Object aggregateValue(Map<String, Object> item, List<PlanRow> rows) {
        String function = string(item.get("function"), "aggregate function");
        Map<String, Object> argument = item.get("argument") == null
            ? null : map(item.get("argument"), "aggregate argument");
        String type = string(item.get("type"), "aggregate type");
        long count = 0;
        Object result = null;
        double averageSum = 0.0;
        for (PlanRow row : rows) {
            Object value = argument == null ? Boolean.TRUE : columnValue(argument, row);
            if (value == null) continue;
            try { count = Math.addExact(count, 1L); }
            catch (ArithmeticException ex) {
                throw error("IntegerOverflow", "aggregate row count overflows INT", item);
            }
            switch (function) {
                case "COUNT" -> { }
                case "SUM" -> {
                    if (type.equals("INT")) {
                        try { result = Math.addExact(result == null ? 0L : (Long) result,
                                                     typed(value, Long.class, "INT")); }
                        catch (ArithmeticException ex) {
                            throw error("IntegerOverflow", "SUM overflows INT", item);
                        }
                    } else {
                        double next = (result == null ? 0.0 : (Double) result)
                            + typed(value, Double.class, "FLOAT");
                        result = finite(next, item);
                    }
                }
                case "AVG" -> {
                    double number = value instanceof Long integer
                        ? integer.doubleValue() : typed(value, Double.class, "FLOAT");
                    averageSum = finite(averageSum + number, item);
                }
                case "MIN", "MAX" -> {
                    if (result == null) result = value;
                    else {
                        int compared = compareValues(value, result,
                            string(argument.get("type"), "aggregate argument type"), "ASC");
                        if ((function.equals("MIN") && compared < 0)
                            || (function.equals("MAX") && compared > 0)) result = value;
                    }
                }
                default -> throw error("InvalidPlan", "unknown aggregate function: " + function, item);
            }
        }
        if (function.equals("COUNT")) return count;
        if (function.equals("AVG")) return count == 0 ? null : finite(averageSum / count, item);
        return result;
    }

    private int compareAggregateRows(AggregateRow left, AggregateRow right,
                                     List<Map<String, Object>> order,
                                     List<Map<String, Object>> groupKeys,
                                     List<Map<String, Object>> items) {
        for (Map<String, Object> orderItem : order) {
            String kind = string(orderItem.get("kind"), "aggregate order kind");
            String direction = string(orderItem.get("direction"), "aggregate order direction");
            Object a, b;
            String type;
            if (kind.equals("output")) {
                int ordinal = Math.toIntExact(longValue(orderItem.get("ordinal"), "output ordinal"));
                if (ordinal < 0 || ordinal >= items.size())
                    throw new EngineException("InvalidPlan", "aggregate output ordinal is out of range");
                a = left.outputValues().get(ordinal);
                b = right.outputValues().get(ordinal);
                Map<String, Object> selected = items.get(ordinal);
                String selectedKind = string(selected.get("kind"), "selected item kind");
                type = selectedKind.equals("column")
                    ? string(map(selected.get("column"), "selected column").get("type"), "column type")
                    : selectedKind.equals("expression")
                        ? string(map(selected.get("expression"), "selected expression").get("type"),
                                 "expression type")
                        : string(selected.get("type"), "aggregate type");
            } else if (kind.equals("group")) {
                int ordinal = referenceIndex(map(orderItem.get("column"), "order group column"), groupKeys);
                a = left.groupValues().get(ordinal);
                b = right.groupValues().get(ordinal);
                type = string(groupKeys.get(ordinal).get("type"), "group key type");
            } else if (kind.equals("expression")) {
                Map<String, Object> expression =
                    map(orderItem.get("expression"), "aggregate order expression");
                a = evaluateAggregate(expression, left.sourceRows());
                b = evaluateAggregate(expression, right.sourceRows());
                type = string(expression.get("type"), "aggregate order expression type");
            } else throw new EngineException("InvalidPlan", "unknown aggregate order kind: " + kind);
            int compared = compareValues(a, b, type, direction);
            if (compared != 0) return compared;
        }
        return 0;
    }

    private int referenceIndex(Map<String, Object> reference, List<Map<String, Object>> references) {
        long table = longValue(reference.get("tableId"), "table id");
        long column = longValue(reference.get("columnId"), "column id");
        long relation = optionalLong(reference.get("relationId"), table);
        for (int i = 0; i < references.size(); i++) {
            Map<String, Object> candidate = references.get(i);
            if (longValue(candidate.get("tableId"), "table id") == table
                && longValue(candidate.get("columnId"), "column id") == column
                && optionalLong(candidate.get("relationId"), table) == relation) {
                if (!Objects.equals(candidate.get("ordinal"), reference.get("ordinal"))
                    || !Objects.equals(candidate.get("type"), reference.get("type")))
                    throw new EngineException("InvalidPlan", "group reference metadata mismatch");
                return i;
            }
        }
        throw new EngineException("InvalidPlan", "ORDER BY column is not a GROUP BY key");
    }

    /** Executes UNION/INTERSECT/EXCEPT, including duplicate-counting ALL semantics. */
    private QueryResult setOperation(Map<String, Object> node) {
        QueryResult left = queryChild(map(node.get("left"), "set left input"));
        QueryResult right = queryChild(map(node.get("right"), "set right input"));
        if (left.columns().size() != right.columns().size())
            throw new EngineException("InvalidPlan", "set inputs have different widths");
        String operation = string(node.get("operation"), "set operation");
        boolean all = optionalBoolean(node.get("all"), false);
        List<List<Object>> rows = new ArrayList<>();
        if (operation.equals("UNION")) {
            rows.addAll(left.rows());
            rows.addAll(right.rows());
            if (!all) rows = distinctRows(rows);
        } else if (operation.equals("INTERSECT")) {
            Map<List<Object>, Integer> available = rowCounts(right.rows());
            Set<List<Object>> emitted = new HashSet<>();
            for (List<Object> row : left.rows()) {
                List<Object> key = new ArrayList<>(row);
                int count = available.getOrDefault(key, 0);
                if (count <= 0 || (!all && !emitted.add(key))) continue;
                rows.add(row);
                if (all) available.put(key, count - 1);
            }
        } else if (operation.equals("EXCEPT")) {
            Map<List<Object>, Integer> excluded = rowCounts(right.rows());
            Set<List<Object>> emitted = new HashSet<>();
            for (List<Object> row : left.rows()) {
                List<Object> key = new ArrayList<>(row);
                int count = excluded.getOrDefault(key, 0);
                if (all && count > 0) {
                    excluded.put(key, count - 1);
                    continue;
                }
                if (!all && count > 0) continue;
                if (all || emitted.add(key)) rows.add(row);
            }
        } else throw new EngineException("InvalidPlan", "unknown set operation: " + operation);
        return new QueryResult(left.columns(), List.copyOf(rows));
    }

    private static Map<List<Object>, Integer> rowCounts(List<List<Object>> rows) {
        Map<List<Object>, Integer> counts = new LinkedHashMap<>();
        for (List<Object> row : rows) {
            List<Object> key = new ArrayList<>(row);
            counts.put(key, counts.getOrDefault(key, 0) + 1);
        }
        return counts;
    }

    /** Query children are statement-shaped nodes embedded by a set, derived table or subquery. */
    private QueryResult queryChild(Map<String, Object> node) {
        ExecutionResult result = executeNode(node);
        if (result instanceof QueryResult query) return query;
        throw new EngineException("InvalidPlan", "subquery plan is not a query");
    }

    private CommandResult update(Map<String, Object> node) {
        TableSchema table = resolveTable(map(node.get("table"), "table"));
        Map<String, Object> inputNode = map(node.get("input"), "Update input");
        if (!Boolean.TRUE.equals(inputNode.get("carriesRowId")))
            throw new EngineException("InvalidPlan", "Update input must carry RowId");
        Map<Long, List<Object>> replacements = new LinkedHashMap<>();
        for (PlanRow oldRow : readInput(inputNode)) {
            List<Object> updated = tableValues(table, oldRow);
            // Every RHS reads oldRow, preserving simultaneous SET a=b,b=a semantics.
            for (Object raw : list(node.get("assignments"), "assignments")) {
                Map<String, Object> assignment = map(raw, "assignment");
                Map<String, Object> target = map(assignment.get("target"), "assignment target");
                int ordinal = Math.toIntExact(longValue(target.get("ordinal"), "ordinal"));
                Map<String, Object> expression =
                    map(assignment.get("value"), "assignment value");
                Object value = evaluate(expression, oldRow);
                value = coerceValue(
                    value, string(target.get("type"), "target type"), expression);
                if (ordinal < 0 || ordinal >= updated.size())
                    throw new EngineException("StorageFailure", "row does not match update schema");
                updated.set(ordinal, value);
            }
            replacements.put(rowIdFor(table, oldRow), updated);
        }
        // 先验证更新后的整张表，再执行写入，避免约束失败留下部分更新。
        List<List<Object>> finalRows = new ArrayList<>();
        for (StoredRow present : records.scan(table.id()))
            finalRows.add(replacements.getOrDefault(present.id(), present.values()));
        validateFinalRows(table, finalRows);
        for (Map.Entry<Long, List<Object>> replacement : replacements.entrySet())
            records.replace(table.id(), replacement.getKey(), replacement.getValue());
        return new CommandResult("UPDATE", replacements.size());
    }

    private CommandResult delete(Map<String, Object> node) {
        TableSchema table = resolveTable(map(node.get("table"), "table"));
        Map<String, Object> inputNode = map(node.get("input"), "Delete input");
        if (!Boolean.TRUE.equals(inputNode.get("carriesRowId")))
            throw new EngineException("InvalidPlan", "Delete input must carry RowId");
        long affected = 0;
        for (PlanRow row : readInput(inputNode)) {
            records.erase(table.id(), rowIdFor(table, row));
            affected++;
        }
        return new CommandResult("DELETE", affected);
    }

    /** Dispatches all relational operators allowed below a statement root. */
    private List<PlanRow> readInput(Map<String, Object> node) {
        String type = string(node.get("type"), "input type");
        if (activeProfiler == null) return readInputRaw(node, type);
        long started = System.nanoTime();
        List<PlanRow> result = readInputRaw(node, type);
        activeProfiler.record(node, result.size(), System.nanoTime() - started);
        return result;
    }

    private List<PlanRow> readInputRaw(Map<String, Object> node, String type) {
        return switch (type) {
            case "SeqScan" -> scan(node);
            case "EmptyResult" -> emptyResult(node);
            case "DerivedTable" -> derivedTable(node);
            case "Filter" -> filter(node);
            case "NestedLoopJoin" -> nestedLoopJoin(node);
            case "GroupBy" -> groupBy(node);
            case "Sort" -> sort(node);
            default -> throw new EngineException("InvalidPlan", "unknown relational input node");
        };
    }

    /** Materializes a child query and gives every output column the derived relation identity. */
    private List<PlanRow> derivedTable(Map<String, Object> node) {
        TableSchema table = decodeTable(map(node.get("table"), "derived table"));
        long relationId = optionalLong(node.get("relationId"), table.id());
        QueryResult query = queryChild(map(node.get("input"), "derived table input"));
        if (query.columns().size() != table.columns().size())
            throw new EngineException("InvalidPlan", "derived query output does not match schema");
        List<ColumnSlot> layout = new ArrayList<>();
        for (int ordinal = 0; ordinal < table.columns().size(); ordinal++) {
            ColumnSchema column = table.columns().get(ordinal);
            layout.add(new ColumnSlot(table.id(), column.id(), relationId, ordinal,
                                      column.type()));
        }
        List<PlanRow> rows = new ArrayList<>();
        for (List<Object> values : query.rows()) {
            if (values.size() != layout.size())
                throw new EngineException("InvalidPlan", "derived query row width is inconsistent");
            rows.add(new PlanRow(Map.of(), layout, new ArrayList<>(values)));
        }
        return rows;
    }

    private List<PlanRow> scan(Map<String, Object> node) {
        TableSchema table = resolveTable(map(node.get("table"), "scan table"));
        long relationId = optionalLong(node.get("relationId"), table.id());
        List<ColumnSlot> layout = scanLayout(node, table, relationId);
        List<PlanRow> result = new ArrayList<>();
        for (StoredRow row : records.scan(table.id())) {
            if (row.values().size() != table.columns().size())
                throw new EngineException("StorageFailure", "stored row does not match table schema");
            // 存储层仍保存完整记录；执行层只物化优化器请求的列。
            List<Object> values = new ArrayList<>();
            for (ColumnSlot slot : layout) values.add(row.values().get(slot.ordinal()));
            result.add(new PlanRow(Map.of(relationId, row.id()), layout, values));
        }
        return result;
    }

    private List<PlanRow> emptyResult(Map<String, Object> node) {
        // 即使没有数据行，也要在执行边界验证优化器保留的身份布局。
        emptyLayout(node);
        return List.of();
    }

    /**
     * Converts the optional SeqScan.columns contract into a checked runtime layout.
     * A missing/null field means all columns for old plans; an empty array is valid.
     */
    private List<ColumnSlot> scanLayout(Map<String, Object> node, TableSchema table,
                                        long relationId) {
        List<ColumnSlot> layout = new ArrayList<>();
        Object selected = node.get("columns");
        if (selected == null) {
            for (int ordinal = 0; ordinal < table.columns().size(); ordinal++) {
                ColumnSchema column = table.columns().get(ordinal);
                layout.add(new ColumnSlot(table.id(), column.id(), relationId,
                                          ordinal, column.type()));
            }
            return layout;
        }
        for (Map<String, Object> reference :
                objectList(selected, "scan columns", "scan column")) {
            ColumnSlot slot = slotFromReference(reference);
            if (slot.tableId() != table.id() || slot.relationId() != relationId
                || slot.ordinal() < 0 || slot.ordinal() >= table.columns().size())
                throw new EngineException("InvalidPlan",
                    "scan column does not belong to the requested relation");
            ColumnSchema column = table.columns().get(slot.ordinal());
            if (slot.columnId() != column.id() || !slot.type().equals(column.type()))
                throw new EngineException("InvalidPlan",
                    "scan column metadata does not match catalog");
            if (layout.contains(slot))
                throw new EngineException("InvalidPlan", "scan column is duplicated");
            layout.add(slot);
        }
        return layout;
    }

    private List<PlanRow> filter(Map<String, Object> node) {
        Map<String, Object> predicate = map(node.get("predicate"), "predicate");
        List<PlanRow> selected = new ArrayList<>();
        for (PlanRow row : readInput(map(node.get("input"), "Filter input")))
            if (predicateTrue(evaluate(predicate, row), predicate)) selected.add(row);
        return selected;
    }

    /** Nested loops preserve left-major order and add NULL-extended rows for outer joins. */
    private List<PlanRow> nestedLoopJoin(Map<String, Object> node) {
        Map<String, Object> leftNode = map(node.get("left"), "join left input");
        Map<String, Object> rightNode = map(node.get("right"), "join right input");
        List<PlanRow> leftRows = readInput(leftNode);
        List<PlanRow> rightRows = readInput(rightNode);
        Map<String, Object> predicate = map(node.get("predicate"), "join predicate");
        String joinType = node.get("joinType") == null
            ? "INNER" : string(node.get("joinType"), "join type");
        if (!List.of("INNER", "LEFT", "RIGHT", "FULL").contains(joinType))
            throw new EngineException("InvalidPlan", "unknown join type: " + joinType);
        List<PlanRow> joined = new ArrayList<>();
        boolean[] rightMatched = new boolean[rightRows.size()];
        for (PlanRow left : leftRows) {
            boolean leftMatched = false;
            for (int index = 0; index < rightRows.size(); index++) {
                PlanRow right = rightRows.get(index);
                PlanRow candidate = combine(left, right);
                if (predicateTrue(evaluate(predicate, candidate), predicate)) {
                    joined.add(candidate);
                    leftMatched = true;
                    rightMatched[index] = true;
                }
            }
            if (!leftMatched && (joinType.equals("LEFT") || joinType.equals("FULL")))
                joined.add(combine(left, nullRowFor(rightNode)));
        }
        if (joinType.equals("RIGHT") || joinType.equals("FULL"))
            for (int index = 0; index < rightRows.size(); index++)
                if (!rightMatched[index])
                    joined.add(combine(nullRowFor(leftNode), rightRows.get(index)));
        return joined;
    }

    /** Reconstructs an empty input layout so an outer join can NULL-extend an empty side. */
    private PlanRow nullRowFor(Map<String, Object> node) {
        List<ColumnSlot> layout = layoutFor(node);
        List<Object> values = new ArrayList<>();
        for (int i = 0; i < layout.size(); i++) values.add(null);
        return new PlanRow(Map.of(), layout, values);
    }

    private List<ColumnSlot> layoutFor(Map<String, Object> node) {
        String type = string(node.get("type"), "layout node type");
        if (type.equals("EmptyResult")) return emptyLayout(node);
        if (type.equals("SeqScan")) {
            TableSchema table = resolveTable(map(node.get("table"), "layout table"));
            long relationId = optionalLong(node.get("relationId"), table.id());
            return scanLayout(node, table, relationId);
        }
        if (type.equals("DerivedTable")) {
            TableSchema table = decodeTable(map(node.get("table"), "derived table"));
            long relationId = optionalLong(node.get("relationId"), table.id());
            List<ColumnSlot> layout = new ArrayList<>();
            for (int ordinal = 0; ordinal < table.columns().size(); ordinal++) {
                ColumnSchema column = table.columns().get(ordinal);
                layout.add(new ColumnSlot(table.id(), column.id(), relationId, ordinal,
                                          column.type()));
            }
            return layout;
        }
        if (type.equals("NestedLoopJoin")) {
            List<ColumnSlot> layout = new ArrayList<>(
                layoutFor(map(node.get("left"), "layout left input")));
            layout.addAll(layoutFor(map(node.get("right"), "layout right input")));
            return layout;
        }
        if (type.equals("Filter") || type.equals("Sort"))
            return layoutFor(map(node.get("input"), "layout input"));
        if (type.equals("GroupBy")) {
            List<ColumnSlot> layout = new ArrayList<>();
            for (Map<String, Object> key : objectList(node.get("keys"), "group keys", "group key"))
                layout.add(slotFromReference(key));
            return layout;
        }
        throw new EngineException("InvalidPlan", "cannot derive layout for " + type);
    }

    /** Validates the retained identity layout used when an outer join NULL-extends an empty side. */
    private List<ColumnSlot> emptyLayout(Map<String, Object> node) {
        List<ColumnSlot> layout = new ArrayList<>();
        List<Map<String, Object>> columns = objectList(
            node.get("columns"), "empty-result columns", "empty-result column");
        if (columns.size() != list(node.get("output"), "empty-result output").size())
            throw new EngineException("InvalidPlan",
                "empty-result columns must match output metadata");
        List<Map<String, Object>> relations = objectList(
            node.get("relations"), "empty-result relations", "empty-result relation");
        if (relations.isEmpty())
            throw new EngineException("InvalidPlan",
                "empty-result must retain at least one relation");
        for (Map<String, Object> relation : relations) {
            TableSchema table = resolveTable(
                map(relation.get("table"), "empty-result relation table"));
            optionalLong(relation.get("relationId"), table.id());
            if (relation.get("relationName") != null)
                string(relation.get("relationName"), "empty-result relation name");
        }
        for (Map<String, Object> reference : columns) {
            ColumnSlot slot = slotFromReference(reference);
            TableSchema table = tablesById.get(slot.tableId());
            if (table == null || slot.ordinal() < 0 || slot.ordinal() >= table.columns().size())
                throw new EngineException("InvalidPlan",
                    "empty-result column does not match catalog");
            ColumnSchema column = table.columns().get(slot.ordinal());
            if (slot.columnId() != column.id() || !slot.type().equals(column.type()))
                throw new EngineException("InvalidPlan",
                    "empty-result column metadata does not match catalog");
            boolean knownRelation = relations.stream().anyMatch(relation -> {
                Map<String, Object> encoded = map(
                    relation.get("table"), "empty-result relation table");
                return longValue(encoded.get("id"), "table id") == slot.tableId() &&
                    optionalLong(relation.get("relationId"), slot.tableId()) == slot.relationId();
            });
            if (!knownRelation)
                throw new EngineException("InvalidPlan",
                    "empty-result column has no matching relation");
            if (layout.contains(slot))
                throw new EngineException("InvalidPlan", "empty-result column is duplicated");
            layout.add(slot);
        }
        return layout;
    }

    /** Without aggregates GROUP BY keeps the first row for every distinct key tuple. */
    private List<PlanRow> groupBy(Map<String, Object> node) {
        List<Map<String, Object>> keys =
            objectList(node.get("keys"), "group keys", "group key");
        Map<List<Object>, PlanRow> groups = new LinkedHashMap<>();
        for (PlanRow row : readInput(map(node.get("input"), "GroupBy input"))) {
            List<Object> values = new ArrayList<>();
            List<ColumnSlot> layout = new ArrayList<>();
            for (Map<String, Object> key : keys) {
                values.add(columnValue(key, row));
                layout.add(columnSlot(key, row));
            }
            // List equality makes two NULL elements equal, as required by GROUP BY.
            groups.putIfAbsent(new ArrayList<>(values),
                new PlanRow(Map.of(), layout, values));
        }
        return new ArrayList<>(groups.values());
    }

    /** ORDER BY compares items left to right and implements the documented NULL ordering. */
    private List<PlanRow> sort(Map<String, Object> node) {
        List<Map<String, Object>> items =
            objectList(node.get("items"), "sort items", "sort item");
        List<PlanRow> rows =
            new ArrayList<>(readInput(map(node.get("input"), "Sort input")));
        Comparator<PlanRow> comparator = (left, right) -> {
            for (Map<String, Object> item : items) {
                String direction = string(item.get("direction"), "sort direction");
                if (!(direction.equals("ASC") || direction.equals("DESC")))
                    throw new EngineException("InvalidPlan",
                        "unknown sort direction: " + direction);
                String kind = item.get("kind") == null
                    ? "column" : string(item.get("kind"), "sort item kind");
                Object a, b;
                String valueType;
                if (kind.equals("column")) {
                    Map<String, Object> reference = map(item.get("column"), "sort column");
                    a = columnValue(reference, left);
                    b = columnValue(reference, right);
                    valueType = string(reference.get("type"), "sort column type");
                } else if (kind.equals("expression")) {
                    Map<String, Object> expression = map(item.get("expression"), "sort expression");
                    a = evaluate(expression, left);
                    b = evaluate(expression, right);
                    valueType = string(expression.get("type"), "sort expression type");
                } else throw new EngineException("InvalidPlan", "unknown sort item kind: " + kind);
                int compared = compareValues(a, b, valueType, direction);
                if (compared != 0) return compared;
            }
            return 0;
        };
        rows.sort(comparator);
        return rows;
    }

    private PlanRow combine(PlanRow left, PlanRow right) {
        Map<Long, Long> rowIds = new LinkedHashMap<>(left.rowIds());
        for (Map.Entry<Long, Long> entry : right.rowIds().entrySet())
            if (rowIds.putIfAbsent(entry.getKey(), entry.getValue()) != null)
                throw new EngineException("InvalidPlan",
                    "join contains a duplicate relation instance");
        List<ColumnSlot> layout = new ArrayList<>(left.layout());
        layout.addAll(right.layout());
        List<Object> values = new ArrayList<>(left.values());
        values.addAll(right.values());
        return new PlanRow(rowIds, layout, values);
    }

    private Object evaluate(Map<String, Object> expression, PlanRow row) {
        String kind = string(expression.get("kind"), "expression kind");
        return switch (kind) {
            case "literal" -> coerceValue(expression.get("value"),
                string(expression.get("type"), "literal type"), expression);
            case "column" -> columnValue(map(expression.get("column"), "column"), row);
            case "unary" -> unary(string(expression.get("op"), "unary op"),
                evaluate(map(expression.get("operand"), "operand"), row), expression);
            case "binary" -> binary(expression, row);
            case "case" -> evaluateCase(expression, row);
            case "inSubquery" -> inSubquery(expression,
                evaluate(map(expression.get("value"), "IN value"), row), row);
            case "existsSubquery" -> existsSubquery(expression, row);
            case "scalarSubquery" -> scalarSubquery(expression, row);
            case "aggregate" -> throw error("InvalidPlan",
                "aggregate expression requires a group", expression);
            default -> throw error("InvalidPlan",
                "unknown expression kind: " + kind, expression);
        };
    }

    /** Evaluates an expression once per group; aggregate leaves consume every source row. */
    private Object evaluateAggregate(Map<String, Object> expression, List<PlanRow> rows) {
        String kind = string(expression.get("kind"), "aggregate expression kind");
        return switch (kind) {
            case "literal" -> coerceValue(expression.get("value"),
                string(expression.get("type"), "literal type"), expression);
            case "column" -> {
                yield columnValue(map(expression.get("column"), "column"),
                                  aggregateContextRow(rows));
            }
            case "aggregate" -> aggregateValue(expression, rows);
            case "unary" -> unary(string(expression.get("op"), "unary op"),
                evaluateAggregate(map(expression.get("operand"), "operand"), rows), expression);
            case "binary" -> {
                Object left = evaluateAggregate(map(expression.get("left"), "left"), rows);
                String op = string(expression.get("op"), "binary op");
                if (op.equals("And") && Boolean.FALSE.equals(left)) yield false;
                if (op.equals("Or") && Boolean.TRUE.equals(left)) yield true;
                Object right = evaluateAggregate(map(expression.get("right"), "right"), rows);
                yield binaryValues(op, left, right, expression);
            }
            case "case" -> evaluateAggregateCase(expression, rows);
            case "inSubquery" -> inSubquery(expression,
                evaluateAggregate(map(expression.get("value"), "IN value"), rows),
                aggregateContextRow(rows));
            case "existsSubquery" -> existsSubquery(expression, aggregateContextRow(rows));
            case "scalarSubquery" -> scalarSubquery(expression, aggregateContextRow(rows));
            default -> throw error("InvalidPlan",
                "unknown aggregate expression kind: " + kind, expression);
        };
    }

    private Object evaluateCase(Map<String, Object> expression, PlanRow row) {
        Map<String, Object> operandExpression = expression.get("operand") instanceof Map<?, ?>
            ? map(expression.get("operand"), "CASE operand") : null;
        Object operand = operandExpression == null ? null : evaluate(operandExpression, row);
        for (Object raw : list(expression.get("branches"), "CASE branches")) {
            Map<String, Object> branch = map(raw, "CASE branch");
            Object when = evaluate(map(branch.get("when"), "CASE WHEN"), row);
            boolean matches = operandExpression == null
                ? Boolean.TRUE.equals(nullableBoolean(when, expression))
                : operand != null && when != null && equalValues(operand, when);
            if (matches) return evaluate(map(branch.get("then"), "CASE THEN"), row);
        }
        return expression.get("else") instanceof Map<?, ?>
            ? evaluate(map(expression.get("else"), "CASE ELSE"), row) : null;
    }

    private Object evaluateAggregateCase(Map<String, Object> expression, List<PlanRow> rows) {
        Map<String, Object> operandExpression = expression.get("operand") instanceof Map<?, ?>
            ? map(expression.get("operand"), "CASE operand") : null;
        Object operand = operandExpression == null
            ? null : evaluateAggregate(operandExpression, rows);
        for (Object raw : list(expression.get("branches"), "CASE branches")) {
            Map<String, Object> branch = map(raw, "CASE branch");
            Object when = evaluateAggregate(map(branch.get("when"), "CASE WHEN"), rows);
            boolean matches = operandExpression == null
                ? Boolean.TRUE.equals(nullableBoolean(when, expression))
                : operand != null && when != null && equalValues(operand, when);
            if (matches)
                return evaluateAggregate(map(branch.get("then"), "CASE THEN"), rows);
        }
        return expression.get("else") instanceof Map<?, ?>
            ? evaluateAggregate(map(expression.get("else"), "CASE ELSE"), rows) : null;
    }

    private PlanRow aggregateContextRow(List<PlanRow> rows) {
        if (!rows.isEmpty()) return rows.get(0);
        return new PlanRow(Map.of(), List.of(), List.of());
    }

    /** IN follows SQL UNKNOWN rules: NULL comparisons only decide the result after no match. */
    private Object inSubquery(Map<String, Object> expression, Object value, PlanRow row) {
        QueryResult query = runSubquery(expression, row);
        if (query.columns().size() != 1)
            throw error("InvalidPlan", "IN subquery must return one column", expression);
        boolean unknown = false;
        for (List<Object> resultRow : query.rows()) {
            if (resultRow.size() != 1)
                throw error("InvalidPlan", "IN subquery row must contain one value", expression);
            Object candidate = resultRow.get(0);
            if (value == null || candidate == null) unknown = true;
            else if (equalValues(value, candidate))
                return optionalBoolean(expression.get("negated"), false) ? false : true;
        }
        if (unknown) return null;
        return optionalBoolean(expression.get("negated"), false) ? true : false;
    }

    private Object existsSubquery(Map<String, Object> expression, PlanRow row) {
        boolean exists = !runSubquery(expression, row).rows().isEmpty();
        return optionalBoolean(expression.get("negated"), false) ? !exists : exists;
    }

    private Object scalarSubquery(Map<String, Object> expression, PlanRow row) {
        QueryResult query = runSubquery(expression, row);
        if (query.columns().size() != 1)
            throw error("InvalidPlan", "scalar subquery must return one column", expression);
        if (query.rows().isEmpty()) return null;
        if (query.rows().size() != 1)
            throw error("ScalarSubqueryCardinality",
                "scalar subquery returned more than one row", expression);
        if (query.rows().get(0).size() != 1)
            throw error("InvalidPlan", "scalar subquery row must contain one value", expression);
        return query.rows().get(0).get(0);
    }

    /** Installs the current row as the outer scope and restores nesting state on every exit. */
    private QueryResult runSubquery(Map<String, Object> expression, PlanRow row) {
        PlanRow saved = correlationRow;
        correlationRow = mergeCorrelation(saved, row);
        try {
            return queryChild(map(expression.get("plan"), "subquery plan"));
        } finally {
            correlationRow = saved;
        }
    }

    private PlanRow mergeCorrelation(PlanRow outer, PlanRow inner) {
        if (outer == null) return inner;
        List<ColumnSlot> layout = new ArrayList<>(outer.layout());
        List<Object> values = new ArrayList<>(outer.values());
        for (int i = 0; i < inner.layout().size(); i++) {
            ColumnSlot slot = inner.layout().get(i);
            int present = layout.indexOf(slot);
            if (present >= 0) values.set(present, inner.values().get(i));
            else {
                layout.add(slot);
                values.add(inner.values().get(i));
            }
        }
        return new PlanRow(Map.of(), layout, values);
    }

    private Object unary(String op, Object value, Map<String, Object> expression) {
        return switch (op) {
            case "Not" -> value == null ? null : !bool(value, expression);
            // 空值判定不触发布尔/数字强制转换，适用于所有列类型。
            case "IsNull" -> value == null;
            case "IsNotNull" -> value != null;
            case "Negate" -> negate(value, expression);
            default -> throw error("InvalidPlan", "unknown unary operator", expression);
        };
    }

    private Object negate(Object value, Map<String, Object> expression) {
        if (value == null) return null;
        if (value instanceof Double number) return finite(-number, expression);
        try { return Math.negateExact(integer(value, expression)); }
        catch (ArithmeticException ex) {
            throw error("IntegerOverflow", "integer negation overflows INT", expression);
        }
    }

    private Object binary(Map<String, Object> expression, PlanRow row) {
        String op = string(expression.get("op"), "binary op");
        Object left = evaluate(map(expression.get("left"), "left"), row);
        if (op.equals("And") && Boolean.FALSE.equals(left)) return false;
        if (op.equals("Or") && Boolean.TRUE.equals(left)) return true;
        Object right = evaluate(map(expression.get("right"), "right"), row);
        return binaryValues(op, left, right, expression);
    }

    /** Implements SQL three-valued logic: UNKNOWN is represented by Java null. */
    private Object binaryValues(String op, Object left, Object right,
                                Map<String, Object> expression) {
        return switch (op) {
            case "And" -> andValue(left, right, expression);
            case "Or" -> orValue(left, right, expression);
            case "Equal" -> left == null || right == null ? null : equalValues(left, right);
            case "NotEqual" -> left == null || right == null ? null : !equalValues(left, right);
            case "Less" -> left == null || right == null ? null :
                compareExpressionValues(left, right, expression) < 0;
            case "LessEqual" -> left == null || right == null ? null :
                compareExpressionValues(left, right, expression) <= 0;
            case "Greater" -> left == null || right == null ? null :
                compareExpressionValues(left, right, expression) > 0;
            case "GreaterEqual" -> left == null || right == null ? null :
                compareExpressionValues(left, right, expression) >= 0;
            case "Add" -> left == null || right == null ? null : arithmetic("add", left, right, expression);
            case "Subtract" -> left == null || right == null ? null : arithmetic("subtract", left, right, expression);
            case "Multiply" -> left == null || right == null ? null : arithmetic("multiply", left, right, expression);
            case "Divide" -> left == null || right == null ? null : divide(left, right, expression);
            case "Like" -> left == null || right == null ? null : like(
                typed(left, String.class, "VARCHAR"), typed(right, String.class, "VARCHAR"));
            default -> throw error("InvalidPlan", "unknown binary operator", expression);
        };
    }

    private Object andValue(Object left, Object right, Map<String, Object> expression) {
        Boolean a = nullableBoolean(left, expression), b = nullableBoolean(right, expression);
        if (Boolean.FALSE.equals(a) || Boolean.FALSE.equals(b)) return false;
        return a == null || b == null ? null : true;
    }

    private Object orValue(Object left, Object right, Map<String, Object> expression) {
        Boolean a = nullableBoolean(left, expression), b = nullableBoolean(right, expression);
        if (Boolean.TRUE.equals(a) || Boolean.TRUE.equals(b)) return true;
        return a == null || b == null ? null : false;
    }

    /** '%' matches any code-point sequence and '_' exactly one Unicode code point. */
    private static boolean like(String value, String pattern) {
        int[] text = value.codePoints().toArray(), wildcard = pattern.codePoints().toArray();
        boolean[] previous = new boolean[text.length + 1];
        previous[0] = true;
        for (int token : wildcard) {
            boolean[] current = new boolean[text.length + 1];
            if (token == '%') current[0] = previous[0];
            for (int index = 1; index <= text.length; index++) {
                if (token == '%') current[index] = previous[index] || current[index - 1];
                else if (token == '_' || token == text[index - 1]) current[index] = previous[index - 1];
            }
            previous = current;
        }
        return previous[text.length];
    }

    private Object arithmetic(String operation, Object left, Object right,
                              Map<String, Object> expression) {
        if (left instanceof Double a && right instanceof Double b) {
            double result = switch (operation) {
                case "add" -> a + b;
                case "subtract" -> a - b;
                default -> a * b;
            };
            return finite(result, expression);
        }
        try {
            long a = integer(left, expression), b = integer(right, expression);
            return switch (operation) {
                case "add" -> Math.addExact(a, b);
                case "subtract" -> Math.subtractExact(a, b);
                default -> Math.multiplyExact(a, b);
            };
        } catch (ArithmeticException ex) {
            throw error("IntegerOverflow",
                "integer " + operation + " overflows INT", expression);
        }
    }

    private Object divide(Object left, Object right, Map<String, Object> expression) {
        if (left instanceof Double a && right instanceof Double b) {
            if (b == 0.0) throw error("DivisionByZero", "division by zero", expression);
            return finite(a / b, expression);
        }
        long a = integer(left, expression), b = integer(right, expression);
        if (b == 0) throw error("DivisionByZero", "division by zero", expression);
        if (a == Long.MIN_VALUE && b == -1)
            throw error("IntegerOverflow", "integer division overflows INT", expression);
        return a / b;
    }

    private int compareExpressionValues(Object left, Object right,
                                        Map<String, Object> expression) {
        if (left instanceof Long a && right instanceof Long b) return Long.compare(a, b);
        if (left instanceof Double a && right instanceof Double b) return compareDouble(a, b);
        throw error("InvalidPlan", "comparison requires equal numeric types", expression);
    }

    private int compareValues(Object left, Object right, String type, String direction) {
        if (left == null || right == null) {
            if (left == right) return 0;
            return left == null
                ? (direction.equals("ASC") ? 1 : -1)
                : (direction.equals("ASC") ? -1 : 1);
        }
        int result = switch (type) {
            case "INT" -> Long.compare(
                typed(left, Long.class, type), typed(right, Long.class, type));
            case "FLOAT" -> compareDouble(
                typed(left, Double.class, type), typed(right, Double.class, type));
            case "VARCHAR" -> compareUtf8(
                typed(left, String.class, type), typed(right, String.class, type));
            case "BOOL" -> Boolean.compare(
                typed(left, Boolean.class, type), typed(right, Boolean.class, type));
            default -> throw new EngineException("InvalidPlan",
                "unsupported sort type: " + type);
        };
        return direction.equals("ASC") ? result : -result;
    }

    private Object columnValue(Map<String, Object> reference, PlanRow row) {
        ColumnSlot slot = columnSlot(reference, row);
        int position = row.layout().indexOf(slot);
        PlanRow source = row;
        if (position < 0 && correlationRow != null) {
            position = correlationRow.layout().indexOf(slot);
            source = correlationRow;
        }
        if (position < 0 || position >= source.values().size())
            throw new EngineException("StorageFailure",
                "row does not match operator layout");
        return source.values().get(position);
    }

    private ColumnSlot columnSlot(Map<String, Object> reference, PlanRow row) {
        long tableId = longValue(reference.get("tableId"), "column table id");
        long columnId = longValue(reference.get("columnId"), "column id");
        long relationId = optionalLong(reference.get("relationId"), tableId);
        int ordinal = Math.toIntExact(longValue(reference.get("ordinal"), "column ordinal"));
        String type = string(reference.get("type"), "column type");
        for (ColumnSlot slot : row.layout()) {
            if (slot.tableId() == tableId && slot.columnId() == columnId
                && slot.relationId() == relationId) {
                if (slot.ordinal() != ordinal || !slot.type().equals(type))
                    throw new EngineException("InvalidPlan",
                        "column reference metadata does not match catalog");
                return slot;
            }
        }
        if (correlationRow != null && correlationRow != row)
            for (ColumnSlot slot : correlationRow.layout()) {
                if (slot.tableId() == tableId && slot.columnId() == columnId
                    && slot.relationId() == relationId) {
                    if (slot.ordinal() != ordinal || !slot.type().equals(type))
                        throw new EngineException("InvalidPlan",
                            "correlated column metadata does not match catalog");
                    return slot;
                }
            }
        throw new EngineException("InvalidPlan",
            "column is not available from the input plan");
    }

    private static ColumnSlot slotFromReference(Map<String, Object> reference) {
        long tableId = longValue(reference.get("tableId"), "column table id");
        return new ColumnSlot(tableId,
            longValue(reference.get("columnId"), "column id"),
            optionalLong(reference.get("relationId"), tableId),
            Math.toIntExact(longValue(reference.get("ordinal"), "column ordinal")),
            string(reference.get("type"), "column type"));
    }

    private List<Object> normalizeRow(TableSchema table, List<Object> raw,
                                      Map<String, Object> expression) {
        if (raw.size() != table.columns().size())
            throw new EngineException("InvalidPlan", "row does not cover table schema");
        List<Object> values = new ArrayList<>();
        for (int ordinal = 0; ordinal < raw.size(); ordinal++) {
            ColumnSchema column = table.columns().get(ordinal);
            Object value = coerceValue(raw.get(ordinal), column.type(), expression);
            validateColumnValue(column, value, true, expression);
            values.add(value);
        }
        return values;
    }

    /** Verifies NOT NULL, VARCHAR(n), PRIMARY KEY and UNIQUE against the final table image. */
    private void validateFinalRows(TableSchema table, List<List<Object>> rows) {
        List<Set<Object>> uniqueValues = new ArrayList<>();
        for (int ordinal = 0; ordinal < table.columns().size(); ordinal++)
            uniqueValues.add(new HashSet<>());
        List<Set<List<Object>>> tableKeys = new ArrayList<>();
        for (TableConstraint constraint : table.constraints()) {
            if (!constraint.kind().equals("PRIMARY_KEY") && !constraint.kind().equals("UNIQUE"))
                throw new EngineException("InvalidPlan", "unknown table constraint");
            if (constraint.columns().isEmpty())
                throw new EngineException("InvalidPlan", "table constraint requires columns");
            tableKeys.add(new HashSet<>());
        }
        for (List<Object> row : rows) {
            if (row.size() != table.columns().size())
                throw new EngineException("StorageFailure", "stored row does not match table schema");
            for (int ordinal = 0; ordinal < row.size(); ordinal++) {
                ColumnSchema column = table.columns().get(ordinal);
                Object value = coerceValue(row.get(ordinal), column.type(), null);
                validateColumnValue(column, value, true, null);
                if (column.unique() && value != null
                    && !uniqueValues.get(ordinal).add(value))
                    throw new EngineException("ConstraintViolation",
                        "duplicate value for unique column " + table.name() + "." + column.name());
            }
            for (int index = 0; index < table.constraints().size(); index++) {
                TableConstraint constraint = table.constraints().get(index);
                List<Object> key = new ArrayList<>();
                boolean containsNull = false;
                for (int ordinal : constraint.columns()) {
                    if (ordinal < 0 || ordinal >= row.size())
                        throw new EngineException("InvalidPlan",
                            "table constraint column is out of range");
                    Object value = row.get(ordinal);
                    containsNull |= value == null;
                    key.add(value);
                }
                if (constraint.kind().equals("PRIMARY_KEY") && containsNull)
                    throw new EngineException("ConstraintViolation",
                        "PRIMARY KEY does not allow NULL");
                // SQL UNIQUE permits multiple tuples when any member is NULL.
                if (!containsNull && !tableKeys.get(index).add(key))
                    throw new EngineException("ConstraintViolation",
                        "duplicate value for table " + constraint.kind());
            }
        }
    }

    private static void validateColumnValue(ColumnSchema column, Object value,
                                            boolean present,
                                            Map<String, Object> expression) {
        if (!present) return;
        if (value == null && column.notNull())
            throw error("ConstraintViolation",
                "column " + column.name() + " does not allow NULL", expression);
        if (value instanceof String text && column.varcharLength() != null
            && text.codePointCount(0, text.length()) > column.varcharLength())
            throw error("ConstraintViolation",
                "value exceeds VARCHAR(" + column.varcharLength() + ") for " + column.name(), expression);
    }

    private static List<List<Object>> distinctRows(List<List<Object>> rows) {
        Map<List<Object>, List<Object>> unique = new LinkedHashMap<>();
        for (List<Object> row : rows) unique.putIfAbsent(new ArrayList<>(row), row);
        return new ArrayList<>(unique.values());
    }

    private static List<List<Object>> window(List<List<Object>> rows, Map<String, Object> node) {
        int from = windowStart(rows.size(), node);
        int to = windowEnd(rows.size(), from, node);
        return new ArrayList<>(rows.subList(from, to));
    }

    private static List<AggregateRow> windowAggregateRows(
        List<AggregateRow> rows, Map<String, Object> node) {
        int from = windowStart(rows.size(), node);
        int to = windowEnd(rows.size(), from, node);
        return new ArrayList<>(rows.subList(from, to));
    }

    private static int windowStart(int size, Map<String, Object> node) {
        long offset = node.get("offset") == null ? 0 : longValue(node.get("offset"), "offset");
        if (offset < 0) throw new EngineException("InvalidPlan", "OFFSET must be non-negative");
        return (int) Math.min(offset, size);
    }

    private static int windowEnd(int size, int from, Map<String, Object> node) {
        if (node.get("limit") == null) return size;
        long limit = longValue(node.get("limit"), "limit");
        if (limit < 0) throw new EngineException("InvalidPlan", "LIMIT must be non-negative");
        return from + (int) Math.min(limit, (long) size - from);
    }

    private List<Object> tableValues(TableSchema table, PlanRow row) {
        List<Object> values = new ArrayList<>();
        long relationId = row.rowIds().containsKey(0L) ? 0 : table.id();
        for (int ordinal = 0; ordinal < table.columns().size(); ordinal++) {
            ColumnSchema column = table.columns().get(ordinal);
            values.add(columnValue(ref(table.id(), column, relationId, ordinal), row));
        }
        return values;
    }

    private long rowIdFor(TableSchema table, PlanRow row) {
        Long rowId = row.rowIds().get(0L);
        // protocolVersion=1 plans emitted before relationId used tableId as the row key.
        if (rowId == null) rowId = row.rowIds().get(table.id());
        if (rowId == null)
            throw new EngineException("InvalidPlan",
                "input does not carry target table RowId");
        return rowId;
    }

    private TableSchema resolveTable(Map<String, Object> encoded) {
        long id = longValue(encoded.get("id"), "table id");
        TableSchema actual = tablesById.get(id);
        if (actual == null
            || !actual.name().equals(string(encoded.get("name"), "table name")))
            throw new EngineException("TableNotFound",
                "table does not exist in engine catalog");
        return actual;
    }

    /** Decodes synthetic derived-table metadata without requiring a physical catalog entry. */
    private TableSchema decodeTable(Map<String, Object> encoded) {
        long id = longValue(encoded.get("id"), "table id");
        String name = normalize(string(encoded.get("name"), "table name"));
        List<ColumnSchema> columns = new ArrayList<>();
        for (Object raw : list(encoded.get("columns"), "table columns")) {
            Map<String, Object> column = map(raw, "table column");
            columns.add(new ColumnSchema(
                longValue(column.get("id"), "column id"),
                normalize(string(column.get("name"), "column name")),
                string(column.get("type"), "column type"),
                nodeLong(column.get("varcharLength")),
                optionalBoolean(column.get("primaryKey"), false),
                optionalBoolean(column.get("notNull"), false),
                optionalBoolean(column.get("unique"), false),
                column.get("defaultValue"),
                optionalBoolean(column.get("hasDefault"), false)));
        }
        return new TableSchema(id, name, List.copyOf(columns));
    }

    /** NULL is accepted by the type coercer; SQL constraints are checked separately. */
    private static Object coerceValue(Object value, String type,
                                      Map<String, Object> expression) {
        if (value == null) return null;
        // JSON has one numeric grammar. A SQL FLOAT such as 95.0 may arrive as Long 95.
        if (type.equals("FLOAT") && value instanceof Long number)
            return number.doubleValue();
        // Canonical zero makes -0.0 and 0.0 equal in equality, grouping and sorting.
        if (type.equals("FLOAT") && value instanceof Double number && number == 0.0)
            return 0.0;
        boolean valid = switch (type) {
            case "INT" -> value instanceof Long;
            case "FLOAT" -> value instanceof Double;
            case "VARCHAR" -> value instanceof String;
            case "BOOL" -> value instanceof Boolean;
            default -> false;
        };
        if (!valid) throw error("TypeMismatch",
            "value does not match " + type, expression);
        return value;
    }

    private static double finite(double value, Map<String, Object> expression) {
        if (!Double.isFinite(value))
            throw error("FloatOverflow",
                "floating-point result is not finite", expression);
        return value;
    }

    private static boolean equalValues(Object left, Object right) {
        if (left instanceof Double a && right instanceof Double b) return a.doubleValue() == b;
        return Objects.equals(left, right);
    }

    private static int compareDouble(double left, double right) {
        return left < right ? -1 : left > right ? 1 : 0;
    }

    /** VARCHAR ordering follows unsigned UTF-8 bytes, matching the protocol's byte order. */
    private static int compareUtf8(String left, String right) {
        byte[] a = left.getBytes(StandardCharsets.UTF_8);
        byte[] b = right.getBytes(StandardCharsets.UTF_8);
        for (int i = 0; i < Math.min(a.length, b.length); i++) {
            int compared = Integer.compare(Byte.toUnsignedInt(a[i]), Byte.toUnsignedInt(b[i]));
            if (compared != 0) return compared;
        }
        return Integer.compare(a.length, b.length);
    }

    private static long integer(Object value, Map<String, Object> expression) {
        if (value instanceof Long number) return number;
        throw error("InvalidPlan", "operator requires INT", expression);
    }

    private static boolean bool(Object value, Map<String, Object> expression) {
        if (value instanceof Boolean truth) return truth;
        throw error("InvalidPlan", "operator requires BOOL", expression);
    }

    private static boolean predicateTrue(Object value, Map<String, Object> expression) {
        return value != null && bool(value, expression);
    }

    private static Boolean nullableBoolean(Object value, Map<String, Object> expression) {
        if (value == null) return null;
        return bool(value, expression);
    }

    private static <T> T typed(Object value, Class<T> expected, String type) {
        if (expected.isInstance(value)) return expected.cast(value);
        throw new EngineException("StorageFailure",
            "stored value does not match " + type);
    }

    private static Map<String, Object> ref(
        long tableId, ColumnSchema column, long relationId, int ordinal) {
        Map<String, Object> result = new LinkedHashMap<>();
        result.put("tableId", tableId);
        result.put("columnId", column.id());
        result.put("relationId", relationId);
        result.put("ordinal", (long) ordinal);
        result.put("type", column.type());
        return result;
    }

    private static List<Map<String, Object>> objectList(
        Object value, String listLabel, String itemLabel) {
        List<Map<String, Object>> result = new ArrayList<>();
        for (Object item : list(value, listLabel)) result.add(map(item, itemLabel));
        return result;
    }

    private static String normalize(String name) {
        return name.toLowerCase(Locale.ROOT);
    }
    @SuppressWarnings("unchecked")
    private static Map<String, Object> map(Object value, String label) {
        if (value instanceof Map<?, ?> raw) return (Map<String, Object>) raw;
        throw new EngineException("ProtocolError", label + " must be an object");
    }
    @SuppressWarnings("unchecked")
    private static List<Object> list(Object value, String label) {
        if (value instanceof List<?> raw) return (List<Object>) raw;
        throw new EngineException("ProtocolError", label + " must be an array");
    }
    private static String string(Object value, String label) {
        if (value instanceof String text) return text;
        throw new EngineException("ProtocolError", label + " must be a string");
    }
    private static long longValue(Object value, String label) {
        if (value instanceof Long number) return number;
        throw new EngineException("ProtocolError", label + " must be an integer");
    }
    private static long optionalLong(Object value, long fallback) {
        return value == null ? fallback : longValue(value, "optional integer");
    }
    private static Long nodeLong(Object value) {
        return value == null ? null : longValue(value, "optional integer");
    }
    private static boolean optionalBoolean(Object value, boolean fallback) {
        if (value == null) return fallback;
        if (value instanceof Boolean truth) return truth;
        throw new EngineException("ProtocolError", "optional boolean must be a boolean");
    }
    private static EngineException error(
        String code, String message, Map<String, Object> expression) {
        if (expression != null && expression.get("span") instanceof Map<?, ?> raw
            && raw.get("begin") instanceof Map<?, ?> begin) {
            Object line = begin.get("line"), column = begin.get("column");
            return new EngineException(code, message,
                line instanceof Long l ? l : null,
                column instanceof Long c ? c : null);
        }
        return new EngineException(code, message);
    }
}
