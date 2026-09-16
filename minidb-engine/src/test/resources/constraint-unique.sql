CREATE TABLE constrained(id INT PRIMARY KEY, code VARCHAR(4) UNIQUE NOT NULL);
INSERT INTO constrained VALUES (1, 'A'), (2, 'B');
-- 多行写入先整体检查；失败时不能插入其中任何一行。
INSERT INTO constrained VALUES (3, 'C'), (4, 'A');
