package minidb;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Comparator;
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

    /** Identifies a column and relation instance; ordinal remains its source-table position. */
    private record ColumnSlot(
        long tableId, long columnId, long relationId, int ordinal, String type) { }

    /** Intermediate row with column identity and optional per-relation RowId for modification. */
    private record PlanRow(Map<Long, Long> rowIds, List<ColumnSlot> layout, List<Object> values) { }

    /** Materialized aggregate row keeps hidden GROUP BY values for ORDER BY. */
    private record AggregateRow(List<Object> groupValues, List<Object> outputValues) { }

    private final RecordStore records;
    private final Map<Long, TableSchema> tablesById = new LinkedHashMap<>();
    private final Map<String, TableSchema> tablesByName = new LinkedHashMap<>();
    private long catalogVersion;
    private long nextTableId = 1;

    public DatabaseEngine() { this(new InMemoryRecordStore()); }
    public DatabaseEngine(RecordStore records) { this.records = Objects.requireNonNull(records); }

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
        return switch (type) {
            case "CreateTable" -> createTable(node);
            case "Insert" -> insert(node);
            case "Project" -> project(node);
            case "Aggregate" -> aggregate(node);
            case "Update" -> update(node);
            case "Delete" -> delete(node);
            case "SeqScan", "NestedLoopJoin", "Filter", "GroupBy", "Sort" ->
                throw new EngineException("InvalidPlan", type + " cannot be an execution root");
            default -> throw new EngineException("InvalidPlan", "unknown plan node: " + type);
        };
    }

    private CommandResult createTable(Map<String, Object> node) {
        String name = normalize(string(node.get("tableName"), "tableName"));
        if (tablesByName.containsKey(name))
            throw new EngineException("TableAlreadyExists", "table already exists: " + name);
        List<ColumnSchema> columns = new ArrayList<>();
        for (Object value : list(node.get("columns"), "columns")) {
            Map<String, Object> column = map(value, "column");
            String columnName = normalize(string(column.get("name"), "column name"));
            String type = string(column.get("type"), "column type");
            if (!(type.equals("INT") || type.equals("VARCHAR")
                || type.equals("BOOL") || type.equals("FLOAT")))
                throw new EngineException("UnsupportedType", "unsupported column type: " + type);
            if (columns.stream().anyMatch(existing -> existing.name().equals(columnName)))
                throw new EngineException("DuplicateColumn", "duplicate column: " + columnName);
            columns.add(new ColumnSchema(columns.size() + 1L, columnName, type));
        }
        if (columns.isEmpty())
            throw new EngineException("EmptyColumnList", "table must contain at least one column");
        TableSchema schema = new TableSchema(nextTableId++, name, List.copyOf(columns));
        records.createTable(schema);
        tablesById.put(schema.id(), schema);
        tablesByName.put(name, schema);
        catalogVersion++;
        return new CommandResult("CREATE", 0);
    }

    private CommandResult insert(Map<String, Object> node) {
        TableSchema table = resolveTable(map(node.get("table"), "table"));
        List<Object> values = new ArrayList<>(list(node.get("values"), "values"));
        if (values.size() != table.columns().size())
            throw new EngineException("InvalidPlan", "INSERT values do not cover table schema");
        for (int i = 0; i < values.size(); i++)
            values.set(i, coerceValue(values.get(i), table.columns().get(i).type(), null));
        records.insert(table.id(), values);
        return new CommandResult("INSERT", 1);
    }

    private QueryResult project(Map<String, Object> node) {
        List<PlanRow> input = readInput(map(node.get("input"), "Project input"));
        List<String> names = new ArrayList<>();
        for (Object output : list(node.get("output"), "output"))
            names.add(string(map(output, "output column").get("name"), "output name"));
        List<Map<String, Object>> references =
            objectList(node.get("columns"), "project columns", "column ref");
        if (names.size() != references.size())
            throw new EngineException("InvalidPlan", "Project output does not match selected columns");
        List<List<Object>> rows = new ArrayList<>();
        for (PlanRow row : input) {
            List<Object> selected = new ArrayList<>();
            for (Map<String, Object> reference : references)
                selected.add(columnValue(reference, row));
            rows.add(selected);
        }
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

        List<AggregateRow> aggregateRows = new ArrayList<>();
        for (Map.Entry<List<Object>, List<PlanRow>> group : groups.entrySet()) {
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
                } else {
                    throw new EngineException("InvalidPlan", "unknown aggregate item kind: " + kind);
                }
            }
            // ArrayList allows SQL NULL elements; List.copyOf deliberately rejects them.
            aggregateRows.add(new AggregateRow(new ArrayList<>(group.getKey()), values));
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
            } else throw new EngineException("InvalidPlan", "unknown aggregate order kind");
        }
        aggregateRows.sort((left, right) -> compareAggregateRows(left, right, order, groupKeys, items));
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
            if (!kind.equals("aggregate"))
                throw error("InvalidPlan", "unknown aggregate item kind", item);
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
                type = selected.get("kind").equals("column")
                    ? string(map(selected.get("column"), "selected column").get("type"), "column type")
                    : string(selected.get("type"), "aggregate type");
            } else if (kind.equals("group")) {
                int ordinal = referenceIndex(map(orderItem.get("column"), "order group column"), groupKeys);
                a = left.groupValues().get(ordinal);
                b = right.groupValues().get(ordinal);
                type = string(groupKeys.get(ordinal).get("type"), "group key type");
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

    private CommandResult update(Map<String, Object> node) {
        TableSchema table = resolveTable(map(node.get("table"), "table"));
        Map<String, Object> inputNode = map(node.get("input"), "Update input");
        if (!Boolean.TRUE.equals(inputNode.get("carriesRowId")))
            throw new EngineException("InvalidPlan", "Update input must carry RowId");
        long affected = 0;
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
            records.replace(table.id(), rowIdFor(table, oldRow), updated);
            affected++;
        }
        return new CommandResult("UPDATE", affected);
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
        return switch (string(node.get("type"), "input type")) {
            case "SeqScan" -> scan(node);
            case "Filter" -> filter(node);
            case "NestedLoopJoin" -> nestedLoopJoin(node);
            case "GroupBy" -> groupBy(node);
            case "Sort" -> sort(node);
            default -> throw new EngineException("InvalidPlan", "unknown relational input node");
        };
    }

    private List<PlanRow> scan(Map<String, Object> node) {
        TableSchema table = resolveTable(map(node.get("table"), "scan table"));
        long relationId = optionalLong(node.get("relationId"), table.id());
        List<ColumnSlot> layout = new ArrayList<>();
        for (int ordinal = 0; ordinal < table.columns().size(); ordinal++) {
            ColumnSchema column = table.columns().get(ordinal);
            layout.add(new ColumnSlot(
                table.id(), column.id(), relationId, ordinal, column.type()));
        }
        List<PlanRow> result = new ArrayList<>();
        for (StoredRow row : records.scan(table.id())) {
            if (row.values().size() != layout.size())
                throw new EngineException("StorageFailure", "stored row does not match table schema");
            result.add(new PlanRow(Map.of(relationId, row.id()), layout, row.values()));
        }
        return result;
    }

    private List<PlanRow> filter(Map<String, Object> node) {
        Map<String, Object> predicate = map(node.get("predicate"), "predicate");
        List<PlanRow> selected = new ArrayList<>();
        for (PlanRow row : readInput(map(node.get("input"), "Filter input")))
            if (bool(evaluate(predicate, row), predicate)) selected.add(row);
        return selected;
    }

    /** Inner join in stable left-major, right-minor nested-loop order. */
    private List<PlanRow> nestedLoopJoin(Map<String, Object> node) {
        List<PlanRow> leftRows = readInput(map(node.get("left"), "join left input"));
        List<PlanRow> rightRows = readInput(map(node.get("right"), "join right input"));
        Map<String, Object> predicate = map(node.get("predicate"), "join predicate");
        List<PlanRow> joined = new ArrayList<>();
        for (PlanRow left : leftRows) {
            for (PlanRow right : rightRows) {
                PlanRow candidate = combine(left, right);
                if (bool(evaluate(predicate, candidate), predicate)) joined.add(candidate);
            }
        }
        return joined;
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
                Map<String, Object> reference = map(item.get("column"), "sort column");
                String direction = string(item.get("direction"), "sort direction");
                if (!(direction.equals("ASC") || direction.equals("DESC")))
                    throw new EngineException("InvalidPlan",
                        "unknown sort direction: " + direction);
                int compared = compareValues(
                    columnValue(reference, left), columnValue(reference, right),
                    string(reference.get("type"), "sort column type"), direction);
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
            default -> throw error("InvalidPlan",
                "unknown expression kind: " + kind, expression);
        };
    }

    private Object unary(String op, Object value, Map<String, Object> expression) {
        return switch (op) {
            case "Not" -> !bool(value, expression);
            case "Negate" -> negate(value, expression);
            default -> throw error("InvalidPlan", "unknown unary operator", expression);
        };
    }

    private Object negate(Object value, Map<String, Object> expression) {
        if (value instanceof Double number) return finite(-number, expression);
        try { return Math.negateExact(integer(value, expression)); }
        catch (ArithmeticException ex) {
            throw error("IntegerOverflow", "integer negation overflows INT", expression);
        }
    }

    private Object binary(Map<String, Object> expression, PlanRow row) {
        String op = string(expression.get("op"), "binary op");
        Object left = evaluate(map(expression.get("left"), "left"), row);
        if (op.equals("And") && !bool(left, expression)) return false;
        if (op.equals("Or") && bool(left, expression)) return true;
        Object right = evaluate(map(expression.get("right"), "right"), row);
        return switch (op) {
            case "And" -> bool(right, expression);
            case "Or" -> bool(right, expression);
            case "Equal" -> equalValues(left, right);
            case "NotEqual" -> !equalValues(left, right);
            case "Less" -> compareExpressionValues(left, right, expression) < 0;
            case "LessEqual" -> compareExpressionValues(left, right, expression) <= 0;
            case "Greater" -> compareExpressionValues(left, right, expression) > 0;
            case "GreaterEqual" -> compareExpressionValues(left, right, expression) >= 0;
            case "Add" -> arithmetic("add", left, right, expression);
            case "Subtract" -> arithmetic("subtract", left, right, expression);
            case "Multiply" -> arithmetic("multiply", left, right, expression);
            case "Divide" -> divide(left, right, expression);
            default -> throw error("InvalidPlan", "unknown binary operator", expression);
        };
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
        if (position < 0 || position >= row.values().size())
            throw new EngineException("StorageFailure",
                "row does not match operator layout");
        return row.values().get(position);
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
        throw new EngineException("InvalidPlan",
            "column is not available from the input plan");
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

    /** NULL can be stored in every declared type, but cannot be an expression operand yet. */
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
