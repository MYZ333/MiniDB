-- Run this file with: E:\MiniDB\run_minidb.bat
CREATE TABLE student(id INT, name VARCHAR, age INT);
INSERT INTO student VALUES (1, 'Alice', 20);
INSERT INTO student VALUES (2, 'Tom', 16);
SELECT name FROM student WHERE age >= 18;
UPDATE student SET age = age + 1 WHERE id = 1;
DELETE FROM student WHERE id = 2;
SELECT id, name, age FROM student;
