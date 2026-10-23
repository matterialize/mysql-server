-- A realistic application schema for CREATE WORKTREE tests (milestone 2).
-- Plain mysql-client syntax; used by worktree.realistic and by the scripts
-- in ~/Development/mysql-worktrees/scripts. Objects deliberately mix
-- qualified (shop.x) and unqualified references, plus references to another
-- schema (ref), which must stay as they are in a worktree.

CREATE DATABASE IF NOT EXISTS ref;
CREATE TABLE ref.currency (code CHAR(3) PRIMARY KEY, rate DECIMAL(10,4) NOT NULL);
INSERT INTO ref.currency VALUES ('USD', 1.0), ('EUR', 1.08), ('GBP', 1.27), ('JPY', 0.0067);

CREATE DATABASE shop;
USE shop;

CREATE TABLE country (id INT PRIMARY KEY, name VARCHAR(50) NOT NULL, currency CHAR(3) NOT NULL,
  FOREIGN KEY (currency) REFERENCES ref.currency (code));
CREATE TABLE city (id INT PRIMARY KEY, country_id INT NOT NULL, name VARCHAR(50) NOT NULL,
  FOREIGN KEY (country_id) REFERENCES country (id));
CREATE TABLE address (id INT AUTO_INCREMENT PRIMARY KEY, city_id INT NOT NULL, line1 VARCHAR(100),
  postal VARCHAR(16), FOREIGN KEY (city_id) REFERENCES city (id));
CREATE TABLE store (id INT PRIMARY KEY, address_id INT NOT NULL, name VARCHAR(50),
  FOREIGN KEY (address_id) REFERENCES address (id));
CREATE TABLE staff (id INT PRIMARY KEY, store_id INT NOT NULL, name VARCHAR(50), email VARCHAR(80),
  FOREIGN KEY (store_id) REFERENCES store (id));
CREATE TABLE customer (id INT AUTO_INCREMENT PRIMARY KEY, store_id INT NOT NULL, address_id INT,
  name VARCHAR(80) NOT NULL, email VARCHAR(120), created DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  active TINYINT NOT NULL DEFAULT 1, KEY (email),
  FOREIGN KEY (store_id) REFERENCES store (id), FOREIGN KEY (address_id) REFERENCES address (id));
CREATE TABLE category (id INT PRIMARY KEY, parent_id INT NULL, name VARCHAR(50) NOT NULL,
  FOREIGN KEY (parent_id) REFERENCES category (id));
CREATE TABLE supplier (id INT PRIMARY KEY, name VARCHAR(80), country_id INT,
  FOREIGN KEY (country_id) REFERENCES country (id));
CREATE TABLE product (id INT PRIMARY KEY, sku VARCHAR(20) NOT NULL UNIQUE, name VARCHAR(120) NOT NULL,
  price DECIMAL(10,2) NOT NULL, weight_g INT, attrs JSON, KEY (price));
CREATE TABLE product_category (product_id INT, category_id INT, PRIMARY KEY (product_id, category_id),
  FOREIGN KEY (product_id) REFERENCES product (id) ON DELETE CASCADE,
  FOREIGN KEY (category_id) REFERENCES category (id));
CREATE TABLE product_supplier (product_id INT, supplier_id INT, cost DECIMAL(10,2),
  PRIMARY KEY (product_id, supplier_id),
  FOREIGN KEY (product_id) REFERENCES product (id) ON DELETE CASCADE,
  FOREIGN KEY (supplier_id) REFERENCES supplier (id));
CREATE TABLE warehouse (id INT PRIMARY KEY, store_id INT, name VARCHAR(50),
  FOREIGN KEY (store_id) REFERENCES store (id));
CREATE TABLE inventory (product_id INT, warehouse_id INT, qty INT NOT NULL,
  PRIMARY KEY (product_id, warehouse_id),
  FOREIGN KEY (product_id) REFERENCES product (id), FOREIGN KEY (warehouse_id) REFERENCES warehouse (id));
CREATE TABLE tag (id INT PRIMARY KEY, name VARCHAR(30) NOT NULL UNIQUE);
CREATE TABLE product_tag (product_id INT, tag_id INT, PRIMARY KEY (product_id, tag_id),
  FOREIGN KEY (product_id) REFERENCES product (id) ON DELETE CASCADE, FOREIGN KEY (tag_id) REFERENCES tag (id));
CREATE TABLE price_history (id BIGINT AUTO_INCREMENT PRIMARY KEY, product_id INT NOT NULL,
  price DECIMAL(10,2), changed DATETIME NOT NULL, FOREIGN KEY (product_id) REFERENCES product (id));
CREATE TABLE coupon (code VARCHAR(16) PRIMARY KEY, pct INT NOT NULL, expires DATE);
CREATE TABLE cart (id BIGINT AUTO_INCREMENT PRIMARY KEY, customer_id INT NOT NULL,
  created DATETIME NOT NULL, FOREIGN KEY (customer_id) REFERENCES customer (id));
CREATE TABLE cart_item (cart_id BIGINT, product_id INT, qty INT, PRIMARY KEY (cart_id, product_id),
  FOREIGN KEY (cart_id) REFERENCES cart (id) ON DELETE CASCADE, FOREIGN KEY (product_id) REFERENCES product (id));
CREATE TABLE wishlist (id INT AUTO_INCREMENT PRIMARY KEY, customer_id INT NOT NULL, name VARCHAR(50),
  FOREIGN KEY (customer_id) REFERENCES customer (id));
CREATE TABLE wishlist_item (wishlist_id INT, product_id INT, PRIMARY KEY (wishlist_id, product_id),
  FOREIGN KEY (wishlist_id) REFERENCES wishlist (id) ON DELETE CASCADE,
  FOREIGN KEY (product_id) REFERENCES product (id));
CREATE TABLE session (id CHAR(32) PRIMARY KEY, customer_id INT, last_seen DATETIME NOT NULL, data BLOB,
  KEY (last_seen));
CREATE TABLE user_pref (customer_id INT PRIMARY KEY, prefs JSON, FOREIGN KEY (customer_id) REFERENCES customer (id));
CREATE TABLE account (id INT PRIMARY KEY, customer_id INT, balance BIGINT NOT NULL,
  version INT NOT NULL DEFAULT 0, FOREIGN KEY (customer_id) REFERENCES customer (id));
CREATE TABLE ledger (id BIGINT AUTO_INCREMENT PRIMARY KEY, token VARCHAR(48) NOT NULL UNIQUE,
  from_acct INT NOT NULL, to_acct INT NOT NULL, amount INT NOT NULL, memo TEXT,
  FOREIGN KEY (from_acct) REFERENCES account (id), FOREIGN KEY (to_acct) REFERENCES account (id));
CREATE TABLE shipment (id BIGINT AUTO_INCREMENT PRIMARY KEY, order_id BIGINT NOT NULL, carrier VARCHAR(20),
  shipped DATETIME, KEY (order_id));
CREATE TABLE coupon_use (code VARCHAR(16), order_id BIGINT, PRIMARY KEY (code, order_id),
  FOREIGN KEY (code) REFERENCES coupon (code));

-- Partitioned fact tables (InnoDB partitioned tables cannot have FKs).
CREATE TABLE orders (id BIGINT NOT NULL, customer_id INT NOT NULL, order_date DATE NOT NULL,
  status ENUM('new','paid','shipped','cancelled') NOT NULL DEFAULT 'new', total DECIMAL(12,2) NOT NULL DEFAULT 0,
  token VARCHAR(48), PRIMARY KEY (id, order_date), KEY (customer_id), KEY (token))
  PARTITION BY RANGE (YEAR(order_date)) SUBPARTITION BY HASH (id) SUBPARTITIONS 2 (
    PARTITION p2023 VALUES LESS THAN (2024),
    PARTITION p2024 VALUES LESS THAN (2025),
    PARTITION p2025 VALUES LESS THAN (2026),
    PARTITION pmax VALUES LESS THAN MAXVALUE);
CREATE TABLE order_item (order_id BIGINT NOT NULL, line INT NOT NULL, product_id INT NOT NULL,
  qty INT NOT NULL, price DECIMAL(10,2) NOT NULL, PRIMARY KEY (order_id, line), KEY (product_id))
  PARTITION BY HASH (order_id) PARTITIONS 8;
CREATE TABLE payment (id BIGINT NOT NULL, order_id BIGINT NOT NULL, method TINYINT NOT NULL,
  amount DECIMAL(12,2) NOT NULL, paid DATETIME NOT NULL, PRIMARY KEY (id, method), KEY (order_id))
  PARTITION BY LIST (method) (PARTITION card VALUES IN (1, 2), PARTITION bank VALUES IN (3),
    PARTITION wallet VALUES IN (4, 5));
CREATE TABLE stock_movement (id BIGINT AUTO_INCREMENT NOT NULL, product_id INT NOT NULL,
  warehouse_id INT NOT NULL, delta INT NOT NULL, reason VARCHAR(20), at DATETIME NOT NULL,
  PRIMARY KEY (id), KEY (product_id)) PARTITION BY KEY (id) PARTITIONS 4;
CREATE TABLE audit_log (id BIGINT AUTO_INCREMENT PRIMARY KEY, entity VARCHAR(20) NOT NULL,
  entity_id BIGINT NOT NULL, action VARCHAR(20) NOT NULL, token VARCHAR(48), at DATETIME NOT NULL,
  KEY (entity, entity_id), KEY (token));

-- FULLTEXT tables.
CREATE TABLE review (id BIGINT AUTO_INCREMENT PRIMARY KEY, product_id INT NOT NULL, customer_id INT,
  rating TINYINT NOT NULL, title VARCHAR(200), body TEXT, FULLTEXT ft_review (title, body),
  FOREIGN KEY (product_id) REFERENCES product (id));
CREATE TABLE product_description (product_id INT PRIMARY KEY, lang CHAR(2) NOT NULL, body MEDIUMTEXT,
  FULLTEXT (body), FOREIGN KEY (product_id) REFERENCES product (id) ON DELETE CASCADE);

