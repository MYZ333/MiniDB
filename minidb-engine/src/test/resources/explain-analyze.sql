-- Plain EXPLAIN must not create the table; ANALYZE must execute the same DDL.
EXPLAIN CREATE TABLE explain_student(id INT PRIMARY KEY, name VARCHAR(20), age INT);
EXPLAIN ANALYZE CREATE TABLE explain_student(id INT PRIMARY KEY, name VARCHAR(20), age INT);
INSERT INTO explain_student VALUES (1, 'Alice', 20), (2, 'Bob', 17), (3, 'Cara', 22);

EXPLAIN SELECT name FROM explain_student WHERE age >= 18 ORDER BY id;
EXPLAIN ANALYZE SELECT name FROM explain_student WHERE age >= 18 ORDER BY id;

-- These paired statements demonstrate the side-effect boundary of ANALYZE.
EXPLAIN INSERT INTO explain_student VALUES (4, 'Dora', 30);
SELECT COUNT(*) AS rows_after_plain_insert FROM explain_student;
EXPLAIN UPDATE explain_student SET age=age+1 WHERE id=1;
SELECT age FROM explain_student WHERE id=1;
EXPLAIN ANALYZE UPDATE explain_student SET age=age+1 WHERE id=1;
SELECT age FROM explain_student WHERE id=1;
EXPLAIN ANALYZE DELETE FROM explain_student WHERE id=3;
SELECT COUNT(*) AS rows_after_delete FROM explain_student;
