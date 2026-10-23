-- Dimension data for the realistic schema. Set @scale first (1 for tests).
-- Fact rows come from realistic_facts.sql, one id range per call.
USE shop;
SET SESSION cte_max_recursion_depth = 1000000;
CREATE DATABASE IF NOT EXISTS loader;
DROP TABLE IF EXISTS loader.seq;
CREATE TABLE loader.seq (n INT PRIMARY KEY);
INSERT INTO loader.seq WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < 100000) SELECT n FROM s;

INSERT INTO country SELECT n, CONCAT('Country ', n), ELT(1 + n % 4, 'USD', 'EUR', 'GBP', 'JPY') FROM loader.seq WHERE n <= 20;
INSERT INTO city SELECT n, 1 + n % 20, CONCAT('City ', n) FROM loader.seq WHERE n <= 200;
INSERT INTO address (id, city_id, line1, postal) SELECT n, 1 + n % 200, CONCAT(n, ' Main St'), LPAD(n, 5, '0') FROM loader.seq WHERE n <= 5000;
INSERT INTO store SELECT n, n, CONCAT('Store ', n) FROM loader.seq WHERE n <= 10;
INSERT INTO staff SELECT n, 1 + n % 10, CONCAT('Staff ', n), CONCAT('staff', n, '@shop.test') FROM loader.seq WHERE n <= 50;
INSERT INTO warehouse SELECT n, n, CONCAT('WH ', n) FROM loader.seq WHERE n <= 10;
INSERT INTO customer (id, store_id, address_id, name, email, created)
  SELECT n, 1 + n % 10, 1 + n % 5000, CONCAT('Customer ', n), CONCAT('c', n, '@mail.test'), '2023-01-01' + INTERVAL n MINUTE
  FROM loader.seq WHERE n <= LEAST(100000, 2000 * @scale);
INSERT INTO category SELECT n, IF(n > 10, 1 + n % 10, NULL), CONCAT('Category ', n) FROM loader.seq WHERE n <= 60;
INSERT INTO supplier SELECT n, CONCAT('Supplier ', n), 1 + n % 20 FROM loader.seq WHERE n <= 100;
INSERT INTO product SELECT n, CONCAT('SKU', LPAD(n, 7, '0')), CONCAT('Product ', n), 1 + (n * 37) % 500, 100 + n % 900,
  JSON_OBJECT('color', ELT(1 + n % 3, 'red', 'green', 'blue')) FROM loader.seq WHERE n <= 5000;
INSERT INTO product_category SELECT n, 1 + n % 60 FROM loader.seq WHERE n <= 5000;
INSERT INTO product_supplier SELECT n, 1 + n % 100, 1 + (n * 31) % 400 FROM loader.seq WHERE n <= 5000;
INSERT INTO inventory SELECT p.n, w.n, 100000 FROM loader.seq p JOIN loader.seq w ON w.n <= 10 WHERE p.n <= 5000;
INSERT INTO tag SELECT n, CONCAT('tag', n) FROM loader.seq WHERE n <= 100;
INSERT INTO product_tag SELECT n, 1 + n % 100 FROM loader.seq WHERE n <= 5000;
INSERT INTO coupon SELECT CONCAT('SAVE', n), 5 + n % 30, '2030-01-01' FROM loader.seq WHERE n <= 50;
INSERT INTO account SELECT n, 1 + n % 1000, 1000, 0 FROM loader.seq WHERE n <= 2000;
INSERT INTO product_description SELECT n, 'en',
  CONCAT('Product ', n, ' is a ', ELT(1 + n % 5, 'durable', 'lightweight', 'premium', 'compact', 'waterproof'),
         ' item made of ', ELT(1 + n % 4, 'steel', 'bamboo', 'cotton', 'carbon fiber'),
         '. ', REPEAT('It ships worldwide and comes with a warranty. ', 1 + n % 20))
  FROM loader.seq WHERE n <= 5000;
INSERT INTO review (product_id, customer_id, rating, title, body)
  SELECT 1 + n % 5000, 1 + n % 2000, 1 + n % 5, CONCAT('Review ', n),
         CONCAT(ELT(1 + n % 4, 'Great', 'Terrible', 'Decent', 'Amazing'), ' product, ',
                ELT(1 + n % 3, 'fast shipping', 'slow delivery', 'perfect packaging'), '. Order ', n)
  FROM loader.seq WHERE n <= LEAST(100000, 5000 * @scale);
INSERT INTO session SELECT LEFT(SHA2(n, 256), 32), 1 + n % 2000, NOW() - INTERVAL n MINUTE, NULL FROM loader.seq WHERE n <= 2000;

DROP DATABASE loader;
