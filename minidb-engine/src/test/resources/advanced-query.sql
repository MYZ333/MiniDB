CREATE TABLE student(id INT, name VARCHAR, age INT, active BOOL, gpa FLOAT);
CREATE TABLE score(student_id INT, value FLOAT);

INSERT INTO student VALUES (2, 'Bob', 20, FALSE, NULL);
INSERT INTO student VALUES (1, 'Alice', 20, TRUE, 3.5);
INSERT INTO student VALUES (3, 'Carol', 18, TRUE, 4.0);
INSERT INTO student VALUES (4, 'Dave', NULL, TRUE, 3.0);
INSERT INTO score VALUES (1, 88.5);
INSERT INTO score VALUES (1, 95.0);
INSERT INTO score VALUES (2, 91.0);
INSERT INTO score VALUES (4, 91.0);
INSERT INTO score VALUES (5, 99.0);

SELECT student.id, score.value
FROM student JOIN score ON student.id = score.student_id
WHERE score.value > 90.0
ORDER BY score.value DESC, student.id ASC;

SELECT age FROM student GROUP BY age ORDER BY age DESC;
SELECT name FROM student ORDER BY gpa ASC;
SELECT name FROM student ORDER BY gpa DESC;
SELECT name FROM student WHERE active = TRUE ORDER BY id ASC;

CREATE TABLE employee(id INT, name VARCHAR, manager_id INT);
INSERT INTO employee VALUES (1, 'CEO', 1);
INSERT INTO employee VALUES (2, 'Developer', 1);
INSERT INTO employee VALUES (3, 'Intern', 2);
SELECT e.name AS employee_name, m.name manager_name
FROM employee AS e JOIN employee m ON e.manager_id = m.id
ORDER BY employee_name DESC;

-- feature-zhangbo: DML aliases and scalar syntax must work through the real bridge.
UPDATE employee e SET e.name = 'Engineer' WHERE e.id = 2;
DELETE FROM employee AS e WHERE e.id IN (3);
SELECT id, name FROM employee WHERE id BETWEEN 1 AND 2 ORDER BY id;
SELECT id FROM student WHERE gpa IS NULL ORDER BY id;
SELECT id FROM student WHERE age IS NOT NULL AND id NOT BETWEEN 2 AND 3 ORDER BY id;
SELECT id FROM student WHERE name NOT IN ('Alice', 'Bob') ORDER BY id;
SELECT e.id, m.id FROM employee e INNER JOIN employee m ON e.manager_id=m.id ORDER BY e.id;
SELECT (COUNT(*)) AS rows, (SUM(age)) AS total FROM student WHERE age IS NOT NULL;
SELECT id FROM student WHERE NULL IS NULL AND id <> 2 ORDER BY id;
SELECT id FROM student WHERE NULL IS NOT NULL;
