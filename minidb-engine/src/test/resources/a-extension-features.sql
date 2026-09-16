CREATE TABLE IF NOT EXISTS people(
  id INT PRIMARY KEY,
  name VARCHAR(20) NOT NULL,
  age INT,
  active BOOL
);
CREATE TABLE IF NOT EXISTS people(ignored INT);
CREATE TABLE score(student_id INT, value INT);
CREATE TABLE enrollment(
  student_id INT,
  course_id INT,
  grade INT,
  PRIMARY KEY(student_id, course_id),
  UNIQUE(student_id, grade)
);

INSERT INTO people VALUES
  (1, 'Alice', 20, TRUE),
  (2, 'Bob', 17, FALSE),
  (3, 'Carol', 30, TRUE);
INSERT INTO score VALUES (1, 90), (1, 70), (2, 80), (4, 80);
INSERT INTO enrollment VALUES (1, 10, 90), (1, 11, 80);

SELECT name,
       CASE WHEN age >= 18 THEN 'adult' ELSE 'minor' END AS category
FROM people ORDER BY id;
SELECT name FROM people
WHERE id IN (SELECT student_id FROM score WHERE value >= 80)
ORDER BY id;
SELECT name FROM people
WHERE EXISTS (SELECT * FROM score WHERE score.student_id = people.id)
ORDER BY id;
SELECT name,
       (SELECT value FROM score WHERE score.student_id = people.id) AS score
FROM people WHERE id = 2;
SELECT d.name FROM
  (SELECT name, age FROM people WHERE age >= 18) d
WHERE d.age < 30;
SELECT p.name FROM people p JOIN
  (SELECT student_id FROM score WHERE value >= 80) s
ON p.id = s.student_id ORDER BY p.id;
SELECT id FROM people UNION SELECT student_id FROM score;
SELECT id FROM people UNION ALL SELECT student_id FROM score;
SELECT id FROM people INTERSECT ALL SELECT student_id FROM score;
SELECT id FROM people EXCEPT SELECT student_id FROM score;
SELECT name, CASE active WHEN TRUE THEN 'yes' ELSE 'no' END AS enabled
FROM people ORDER BY id;

ALTER TABLE people ADD COLUMN nickname VARCHAR(10) NOT NULL DEFAULT 'n/a';
ALTER TABLE people RENAME COLUMN nickname TO alias;
SELECT name, alias FROM people ORDER BY id;
ALTER TABLE people DROP COLUMN alias;
ALTER TABLE people RENAME TO persons;
SELECT name FROM persons ORDER BY id;
