-- Persistent-safe CLI demonstration. It only resets its own demo_cli_ table.
-- Run this file with: E:\MiniDB\run_minidb.bat
DROP TABLE IF EXISTS demo_cli_student;
CREATE TABLE demo_cli_student(id INT PRIMARY KEY, name VARCHAR, age INT);
INSERT INTO demo_cli_student VALUES (1, 'Alice', 20);
INSERT INTO demo_cli_student VALUES (2, 'Tom', 16);
SELECT name FROM demo_cli_student WHERE age >= 18;
UPDATE demo_cli_student SET age = age + 1 WHERE id = 1;
DELETE FROM demo_cli_student WHERE id = 2;
SELECT id, name, age FROM demo_cli_student;
