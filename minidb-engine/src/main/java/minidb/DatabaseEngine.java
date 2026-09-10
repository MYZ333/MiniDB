package minidb;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Objects;

/** Executes protocolVersion=1 plans emitted by DBcompiler-main/app/plan_json.cpp. */
public final class DatabaseEngine {
    public record ColumnSchema(long id, String name, String type) { }
    public record TableSchema(long id, String name, List<ColumnSchema> columns) { }
    public record StoredRow(long id, List<Object> values) { }
    public record CommandResult(String operation, long affectedRows) implements ExecutionResult { }
    public record QueryResult(List<String> columns, List<List<Object>> rows) implements ExecutionResult { }
    public sealed interface ExecutionResult permits CommandResult, QueryResult { }

    private final RecordStore records;
    private final Map<Long, TableSchema> tablesById = new LinkedHashMap<>();
    private final Map<String, TableSchema> tablesByName = new LinkedHashMap<>();
    private long catalogVersion;
    private long nextTableId = 1;

    public DatabaseEngine() { this(new InMemoryRecordStore()); }
    public DatabaseEngine(RecordStore records) { this.records = Objects.requireNonNull(records); }

    public List<ExecutionResult> executeProgramJson(String json) {
        Map<String, Object> program = map(Json.parse(json), "program");
        if (longValue(program.get("protocolVersion"), "protocolVersion") != 1) throw new EngineException("ProtocolMismatch", "unsupported plan protocol");
        List<ExecutionResult> results = new ArrayList<>();
        for (Object plan : list(program.get("plans"), "plans")) results.add(executePlan(map(plan, "plan")));
        return results;
    }

    public ExecutionResult executePlan(Map<String, Object> plan) {
        if (longValue(plan.get("catalogVersion"), "catalogVersion") != catalogVersion)
            throw new EngineException("CatalogVersionMismatch", "logical plan was compiled against a stale catalog version");
        return executeNode(map(plan.get("root"), "root"), true);
    }

    private ExecutionResult executeNode(Map<String, Object> node, boolean root) {
        String type = string(node.get("type"), "node type");
        return switch (type) {
            case "CreateTable" -> createTable(node);
            case "Insert" -> insert(node);
            case "Project" -> project(node);
            case "Update" -> update(node);
            case "Delete" -> delete(node);
            case "SeqScan", "Filter" -> throw new EngineException("InvalidPlan", type + " cannot be an execution root");
            default -> throw new EngineException("InvalidPlan", "unknown plan node: " + type);
        };
    }

    private CommandResult createTable(Map<String, Object> node) {
        String name = normalize(string(node.get("tableName"), "tableName"));
        if (tablesByName.containsKey(name)) throw new EngineException("TableAlreadyExists", "table already exists: " + name);
        List<ColumnSchema> columns = new ArrayList<>();
        for (Object value : list(node.get("columns"), "columns")) {
            Map<String, Object> column = map(value, "column");
            String columnName = normalize(string(column.get("name"), "column name"));
            String type = string(column.get("type"), "column type");
            if (!(type.equals("INT") || type.equals("VARCHAR"))) throw new EngineException("UnsupportedType", "unsupported column type: " + type);
            if (columns.stream().anyMatch(existing -> existing.name().equals(columnName))) throw new EngineException("DuplicateColumn", "duplicate column: " + columnName);
            columns.add(new ColumnSchema(columns.size() + 1L, columnName, type));
        }
        if (columns.isEmpty()) throw new EngineException("EmptyColumnList", "table must contain at least one column");
        TableSchema schema = new TableSchema(nextTableId++, name, List.copyOf(columns));
        records.createTable(schema); tablesById.put(schema.id(), schema); tablesByName.put(name, schema); catalogVersion++;
        return new CommandResult("CREATE", 0);
    }

    private CommandResult insert(Map<String, Object> node) {
        TableSchema table = resolveTable(map(node.get("table"), "table"));
        List<Object> values = new ArrayList<>(list(node.get("values"), "values"));
        if (values.size() != table.columns().size()) throw new EngineException("InvalidPlan", "INSERT values do not cover table schema");
        for (int i = 0; i < values.size(); i++) ensureType(values.get(i), table.columns().get(i).type(), null);
        records.insert(table.id(), values); return new CommandResult("INSERT", 1);
    }

    private QueryResult project(Map<String, Object> node) {
        List<StoredRow> input = readInput(map(node.get("input"), "Project input"));
        List<String> names = new ArrayList<>();
        for (Object output : list(node.get("output"), "output")) names.add(string(map(output, "output column").get("name"), "output name"));
        List<List<Object>> rows = new ArrayList<>();
        for (StoredRow row : input) {
            List<Object> selected = new ArrayList<>();
            for (Object ref : list(node.get("columns"), "project columns")) selected.add(columnValue(map(ref, "column ref"), row.values()));
            rows.add(selected);
        }
        return new QueryResult(List.copyOf(names), List.copyOf(rows));
    }

    private CommandResult update(Map<String, Object> node) {
        TableSchema table = resolveTable(map(node.get("table"), "table"));
        Map<String, Object> inputNode = map(node.get("input"), "Update input");
        if (!Boolean.TRUE.equals(inputNode.get("carriesRowId"))) throw new EngineException("InvalidPlan", "Update input must carry RowId");
        long affected = 0;
        for (StoredRow oldRow : readInput(inputNode)) {
            List<Object> updated = new ArrayList<>(oldRow.values());
            for (Object raw : list(node.get("assignments"), "assignments")) {
                Map<String, Object> assignment = map(raw, "assignment");
                Map<String, Object> target = map(assignment.get("target"), "assignment target");
                int ordinal = Math.toIntExact(longValue(target.get("ordinal"), "ordinal"));
                Object value = evaluate(map(assignment.get("value"), "assignment value"), oldRow.values());
                ensureType(value, string(target.get("type"), "target type"), map(assignment.get("value"), "assignment value"));
                if (ordinal < 0 || ordinal >= updated.size()) throw new EngineException("StorageFailure", "row does not match update schema");
                updated.set(ordinal, value);
            }
            records.replace(table.id(), oldRow.id(), updated); affected++;
        }
        return new CommandResult("UPDATE", affected);
    }

    private CommandResult delete(Map<String, Object> node) {
        TableSchema table = resolveTable(map(node.get("table"), "table"));
        Map<String, Object> inputNode = map(node.get("input"), "Delete input");
        if (!Boolean.TRUE.equals(inputNode.get("carriesRowId"))) throw new EngineException("InvalidPlan", "Delete input must carry RowId");
        long affected = 0;
        for (StoredRow row : readInput(inputNode)) { records.erase(table.id(), row.id()); affected++; }
        return new CommandResult("DELETE", affected);
    }

    private List<StoredRow> readInput(Map<String, Object> node) {
        return switch (string(node.get("type"), "input type")) {
            case "SeqScan" -> records.scan(resolveTable(map(node.get("table"), "scan table")).id());
            case "Filter" -> {
                List<StoredRow> selected = new ArrayList<>(); Map<String, Object> predicate = map(node.get("predicate"), "predicate");
                for (StoredRow row : readInput(map(node.get("input"), "Filter input"))) if (bool(evaluate(predicate, row.values()), predicate)) selected.add(row);
                yield selected;
            }
            default -> throw new EngineException("InvalidPlan", "input must be SeqScan or Filter");
        };
    }

    private Object evaluate(Map<String, Object> expression, List<Object> row) {
        String kind = string(expression.get("kind"), "expression kind");
        return switch (kind) {
            case "literal" -> expression.get("value");
            case "column" -> columnValue(map(expression.get("column"), "column"), row);
            case "unary" -> unary(string(expression.get("op"), "unary op"), evaluate(map(expression.get("operand"), "operand"), row), expression);
            case "binary" -> binary(expression, row);
            default -> throw error("InvalidPlan", "unknown expression kind: " + kind, expression);
        };
    }

    private Object unary(String op, Object value, Map<String, Object> expression) {
        return switch (op) {
            case "Not" -> !bool(value, expression);
            case "Negate" -> { try { yield Math.negateExact(integer(value, expression)); } catch (ArithmeticException ex) { throw error("IntegerOverflow", "integer negation overflows INT", expression); } }
            default -> throw error("InvalidPlan", "unknown unary operator", expression);
        };
    }

    private Object binary(Map<String, Object> expression, List<Object> row) {
        String op = string(expression.get("op"), "binary op"); Object left = evaluate(map(expression.get("left"), "left"), row);
        if (op.equals("And") && !bool(left, expression)) return false;
        if (op.equals("Or") && bool(left, expression)) return true;
        Object right = evaluate(map(expression.get("right"), "right"), row);
        return switch (op) {
            case "And" -> bool(right, expression); case "Or" -> bool(right, expression);
            case "Equal" -> Objects.equals(left, right); case "NotEqual" -> !Objects.equals(left, right);
            case "Less" -> integer(left, expression) < integer(right, expression);
            case "LessEqual" -> integer(left, expression) <= integer(right, expression);
            case "Greater" -> integer(left, expression) > integer(right, expression);
            case "GreaterEqual" -> integer(left, expression) >= integer(right, expression);
            case "Add" -> exact("add", left, right, expression); case "Subtract" -> exact("subtract", left, right, expression);
            case "Multiply" -> exact("multiply", left, right, expression); case "Divide" -> divide(left, right, expression);
            default -> throw error("InvalidPlan", "unknown binary operator", expression);
        };
    }

    private long exact(String operation, Object left, Object right, Map<String, Object> expression) {
        try { long a = integer(left, expression), b = integer(right, expression); return switch (operation) { case "add" -> Math.addExact(a, b); case "subtract" -> Math.subtractExact(a, b); default -> Math.multiplyExact(a, b); }; }
        catch (ArithmeticException ex) { throw error("IntegerOverflow", "integer " + operation + " overflows INT", expression); }
    }
    private long divide(Object left, Object right, Map<String, Object> expression) {
        long a = integer(left, expression), b = integer(right, expression);
        if (b == 0) throw error("DivisionByZero", "division by zero", expression);
        if (a == Long.MIN_VALUE && b == -1) throw error("IntegerOverflow", "integer division overflows INT", expression);
        return a / b;
    }

    private Object columnValue(Map<String, Object> reference, List<Object> row) {
        int ordinal = Math.toIntExact(longValue(reference.get("ordinal"), "column ordinal"));
        if (ordinal < 0 || ordinal >= row.size()) throw new EngineException("StorageFailure", "row does not match table schema");
        return row.get(ordinal);
    }
    private TableSchema resolveTable(Map<String, Object> encoded) {
        long id = longValue(encoded.get("id"), "table id"); TableSchema actual = tablesById.get(id);
        if (actual == null || !actual.name().equals(string(encoded.get("name"), "table name"))) throw new EngineException("TableNotFound", "table does not exist in engine catalog");
        return actual;
    }
    private static void ensureType(Object value, String type, Map<String, Object> expression) {
        boolean valid = switch (type) { case "INT" -> value instanceof Long; case "VARCHAR" -> value instanceof String; case "BOOL" -> value instanceof Boolean; default -> false; };
        if (!valid) throw error("TypeMismatch", "value does not match " + type, expression);
    }
    private static long integer(Object value, Map<String, Object> expression) { if (value instanceof Long number) return number; throw error("InvalidPlan", "operator requires INT", expression); }
    private static boolean bool(Object value, Map<String, Object> expression) { if (value instanceof Boolean truth) return truth; throw error("InvalidPlan", "operator requires BOOL", expression); }
    private static String normalize(String name) { return name.toLowerCase(Locale.ROOT); }
    @SuppressWarnings("unchecked") private static Map<String, Object> map(Object value, String label) { if (value instanceof Map<?, ?> raw) return (Map<String, Object>) raw; throw new EngineException("ProtocolError", label + " must be an object"); }
    @SuppressWarnings("unchecked") private static List<Object> list(Object value, String label) { if (value instanceof List<?> raw) return (List<Object>) raw; throw new EngineException("ProtocolError", label + " must be an array"); }
    private static String string(Object value, String label) { if (value instanceof String text) return text; throw new EngineException("ProtocolError", label + " must be a string"); }
    private static long longValue(Object value, String label) { if (value instanceof Long number) return number; throw new EngineException("ProtocolError", label + " must be an integer"); }
    private static EngineException error(String code, String message, Map<String, Object> expression) {
        if (expression != null && expression.get("span") instanceof Map<?, ?> raw && raw.get("begin") instanceof Map<?, ?> begin) {
            Object line = begin.get("line"), column = begin.get("column");
            return new EngineException(code, message, line instanceof Long l ? l : null, column instanceof Long c ? c : null);
        }
        return new EngineException(code, message);
    }
}
