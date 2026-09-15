CREATE TABLE person(id INT PRIMARY KEY);
CREATE TABLE score(person_id INT, value INT);
INSERT INTO person VALUES (1);
INSERT INTO score VALUES (1, 80), (1, 90);
SELECT (SELECT value FROM score WHERE score.person_id = person.id) AS value
FROM person;
