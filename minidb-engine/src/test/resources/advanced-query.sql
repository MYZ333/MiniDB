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
