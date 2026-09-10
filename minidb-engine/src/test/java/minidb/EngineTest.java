package minidb;

import java.util.List;

/** Dependency-free integration test; run after test-compile with java -ea. */
public final class EngineTest {
    public static void main(String[] args) {
        String program = """
            {"protocolVersion":1,"plans":[
              {"catalogVersion":0,"root":{"type":"CreateTable","tableName":"student","columns":[{"name":"id","type":"INT"},{"name":"name","type":"VARCHAR"}],"output":[],"carriesRowId":false}},
              {"catalogVersion":1,"root":{"type":"Insert","table":{"id":1,"name":"student","columns":[]},"values":[1,"Alice"],"output":[],"carriesRowId":false}},
              {"catalogVersion":1,"root":{"type":"Project","columns":[{"tableId":1,"columnId":2,"ordinal":1,"type":"VARCHAR"}],"input":{"type":"Filter","predicate":{"type":"BOOL","span":null,"kind":"binary","op":"Equal","left":{"type":"INT","span":null,"kind":"column","column":{"tableId":1,"columnId":1,"ordinal":0,"type":"INT"}},"right":{"type":"INT","span":null,"kind":"literal","value":1}},"input":{"type":"SeqScan","table":{"id":1,"name":"student","columns":[]},"output":[],"carriesRowId":false},"output":[],"carriesRowId":false},"output":[{"name":"name","type":"VARCHAR"}],"carriesRowId":false}},
              {"catalogVersion":1,"root":{"type":"Update","table":{"id":1,"name":"student","columns":[]},"assignments":[{"target":{"tableId":1,"columnId":1,"ordinal":0,"type":"INT"},"value":{"type":"INT","span":null,"kind":"binary","op":"Add","left":{"type":"INT","span":null,"kind":"column","column":{"tableId":1,"columnId":1,"ordinal":0,"type":"INT"}},"right":{"type":"INT","span":null,"kind":"literal","value":1}}}],"input":{"type":"SeqScan","table":{"id":1,"name":"student","columns":[]},"output":[],"carriesRowId":true},"output":[],"carriesRowId":false}}
            ]}
            """;
        List<DatabaseEngine.ExecutionResult> results = new DatabaseEngine().executeProgramJson(program);
        check(results.size() == 4, "all plans must execute");
        DatabaseEngine.QueryResult query = (DatabaseEngine.QueryResult) results.get(2);
        check(query.rows().size() == 1 && query.rows().get(0).get(0).equals("Alice"), "filter/project result mismatch");
        check(((DatabaseEngine.CommandResult) results.get(3)).affectedRows() == 1, "update count mismatch");
        try { new DatabaseEngine().executeProgramJson("{\"protocolVersion\":1,\"plans\":[{\"catalogVersion\":1,\"root\":{\"type\":\"CreateTable\",\"tableName\":\"x\",\"columns\":[{\"name\":\"id\",\"type\":\"INT\"}]}}]}"); throw new AssertionError("stale plan must fail"); }
        catch (EngineException error) { check(error.code().equals("CatalogVersionMismatch"), "wrong stale-plan error"); }
        System.out.println("EngineTest passed");
    }
    private static void check(boolean condition, String message) { if (!condition) throw new AssertionError(message); }
}
