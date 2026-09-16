CREATE TABLE constrained(id INT PRIMARY KEY, code VARCHAR(4) UNIQUE NOT NULL);
INSERT INTO constrained VALUES (1, 'A'), (2, 'B');
-- UPDATE 表达式在编译期只有类型，VARCHAR 长度由执行层在写入前检查。
UPDATE constrained SET code = 'ABCDE' WHERE id = 2;
