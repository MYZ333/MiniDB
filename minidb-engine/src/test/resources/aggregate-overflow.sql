-- SUM(INT) preserves the 64-bit type and reports overflow at the call site.
CREATE TABLE huge(v INT);
INSERT INTO huge VALUES (9223372036854775807);
INSERT INTO huge VALUES (1);
SELECT SUM(v) FROM huge;
