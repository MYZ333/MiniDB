CREATE TABLE pair_value(
  left_id INT,
  right_id INT,
  PRIMARY KEY(left_id, right_id)
);
INSERT INTO pair_value VALUES (1, 2), (1, 2);
