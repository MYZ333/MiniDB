-- 谓词下推应在连接前过滤两侧；列裁剪应排除两个 unused 列和 score.id。
CREATE TABLE opt_student(id INT PRIMARY KEY, name VARCHAR(20), age INT, unused VARCHAR(20));
CREATE TABLE opt_score(id INT PRIMARY KEY, student_id INT, value INT, unused VARCHAR(20));
INSERT INTO opt_student VALUES
    (1, 'Alice', 20, 'left-a'),
    (2, 'Bob', 17, 'left-b'),
    (3, 'Cara', 22, 'left-c');
INSERT INTO opt_score VALUES
    (11, 1, 90, 'right-a'),
    (12, 1, 70, 'right-b'),
    (13, 2, 95, 'right-c'),
    (14, 3, 85, 'right-d'),
    (15, 4, 99, 'right-e');

EXPLAIN ANALYZE
SELECT s.name, sc.value
FROM opt_student AS s JOIN opt_score AS sc ON s.id=sc.student_id
WHERE s.age>=18 AND sc.value>=80
ORDER BY s.name;

SELECT s.name, sc.value
FROM opt_student AS s JOIN opt_score AS sc ON s.id=sc.student_id
WHERE s.age>=18 AND sc.value>=80
ORDER BY s.name;

-- COUNT(*) 只依赖行数，扫描节点不需要物化任何业务列。
EXPLAIN ANALYZE SELECT COUNT(*) AS total FROM opt_student;
SELECT COUNT(*) AS total FROM opt_student;

-- 恒假条件生成 EmptyResult；全局聚合仍必须保留 COUNT(*)=0 的单行边界。
EXPLAIN ANALYZE SELECT name FROM opt_student WHERE 1=0;
SELECT name FROM opt_student WHERE 1=0;
EXPLAIN ANALYZE SELECT name FROM opt_student LIMIT 0;
SELECT name FROM opt_student LIMIT 0;
EXPLAIN ANALYZE SELECT COUNT(*) AS total FROM opt_student WHERE 1=0;
SELECT COUNT(*) AS total FROM opt_student WHERE 1=0;

-- 内层恒假 INNER JOIN 变为空结果后，外层 RIGHT JOIN 仍要用其身份布局补 NULL。
EXPLAIN ANALYZE
SELECT kept.name
FROM opt_student AS s JOIN opt_score AS sc ON 1=0
RIGHT JOIN opt_student AS kept ON sc.student_id=kept.id
ORDER BY kept.id;

SELECT kept.name
FROM opt_student AS s JOIN opt_score AS sc ON 1=0
RIGHT JOIN opt_student AS kept ON sc.student_id=kept.id
ORDER BY kept.id;

-- 修改语句保留根节点并返回影响行数 0；不可达的 UPDATE RHS 不应求值。
UPDATE opt_student SET age=1/0 WHERE 1=0;
DELETE FROM opt_student WHERE 1=0;
SELECT COUNT(*) AS rows_after_empty_dml FROM opt_student;
