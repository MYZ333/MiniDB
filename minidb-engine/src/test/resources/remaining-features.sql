-- A 已经产生这些 AST；本文件验证 B、JSON 协议和 Java 执行层完整接通。
DROP TABLE IF EXISTS absent;
CREATE TABLE users(
    id INT PRIMARY KEY,
    name VARCHAR(5) NOT NULL UNIQUE,
    score INT DEFAULT 7,
    note VARCHAR(8)
);
CREATE TABLE badges(user_id INT, label VARCHAR(8));

INSERT INTO users(id, name, note) VALUES
    (1, 'Alice', NULL),
    (2, 'Bob', 'ok'),
    (3, 'Cara', NULL);
INSERT INTO badges VALUES (1, 'gold'), (4, 'guest');
UPDATE users SET note = NULL WHERE id = 2;

SELECT DISTINCT id + score AS total, name LIKE 'A%' AS begins
FROM users
WHERE note IS NULL
ORDER BY id + score DESC
LIMIT 3 OFFSET 0;

SELECT DISTINCT score FROM users ORDER BY score;

SELECT score, COUNT(*) AS amount, SUM(id) + 1 AS sum_plus
FROM users
GROUP BY score
HAVING COUNT(*) > 1
ORDER BY SUM(id) DESC
LIMIT 3;

SELECT u.id, b.label
FROM users u LEFT JOIN badges b ON u.id = b.user_id
ORDER BY u.id;

SELECT u.name, b.label
FROM users u RIGHT JOIN badges b ON u.id = b.user_id
ORDER BY b.user_id;

SELECT u.name, b.label
FROM users u FULL JOIN badges b ON u.id = b.user_id
ORDER BY b.user_id;

DROP TABLE IF EXISTS badges, absent;
DROP TABLE users;
