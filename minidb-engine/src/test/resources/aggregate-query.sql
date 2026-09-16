CREATE TABLE sales(dept VARCHAR, amount INT, score FLOAT, note VARCHAR, active BOOL);
INSERT INTO sales VALUES ('a', 10, 1.5, 'x', TRUE);
INSERT INTO sales VALUES ('a', 20, NULL, 'y', FALSE);
INSERT INTO sales VALUES ('b', NULL, 2.5, NULL, TRUE);
INSERT INTO sales VALUES ('b', 5, 3.5, 'z', NULL);
INSERT INTO sales VALUES (NULL, 7, 4.5, 'q', TRUE);

SELECT COUNT(*) AS rows, COUNT(amount) AS non_null_amounts,
       SUM(amount) AS total, AVG(amount) AS mean,
       MIN(note) AS first_note, MAX(score) AS max_score
FROM sales;

SELECT dept, COUNT(*) AS rows, SUM(amount) AS total,
       AVG(score) AS mean_score, MIN(note) AS first_note, MAX(note) AS last_note
FROM sales GROUP BY dept ORDER BY total DESC, dept ASC;

SELECT dept, COUNT(amount) AS present
FROM sales GROUP BY dept ORDER BY dept ASC;

SELECT COUNT(*) AS rows, COUNT(amount) AS present, SUM(amount) AS total,
       AVG(score) AS mean_score, MIN(note) AS first_note, MAX(note) AS last_note
FROM sales WHERE dept = 'missing';

SELECT dept, COUNT(*) AS rows
FROM sales WHERE dept = 'missing' GROUP BY dept ORDER BY rows DESC;

-- FLOAT SUM and BOOL ordering reuse the scalar type contract.
SELECT SUM(score), AVG(score), MIN(amount), MAX(amount), MIN(active), MAX(active)
FROM sales;
-- Keep the group key hidden from final projection.
SELECT COUNT(*) AS rows FROM sales GROUP BY dept ORDER BY dept;
-- Both scans read the same physical table; aggregate arguments retain relationId.
SELECT l.dept, SUM(r.amount) AS total
FROM sales l JOIN sales r ON l.note = r.note
GROUP BY l.dept ORDER BY total DESC;

CREATE TABLE empty_values(v INT, f FLOAT);
SELECT COUNT(*), COUNT(v), SUM(v), AVG(v), MIN(v), MAX(v) FROM empty_values;
INSERT INTO empty_values VALUES (NULL, NULL);
SELECT COUNT(*), COUNT(v), SUM(v), AVG(f), MIN(v), MAX(f) FROM empty_values;
SELECT v, COUNT(*), SUM(v) FROM empty_values GROUP BY v;
