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
