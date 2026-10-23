-- Views, routines, triggers and events of the realistic schema (load the
-- tables and data first: triggers would fire on every bulk-loaded row).

USE shop;

-- Views (two are views on views; one joins another schema).
CREATE VIEW v_customer_city AS
  SELECT c.id AS customer_id, c.name, ci.name AS city, co.name AS country, co.currency
  FROM shop.customer c JOIN shop.address a ON a.id = c.address_id
  JOIN shop.city ci ON ci.id = a.city_id JOIN shop.country co ON co.id = ci.country_id;
CREATE VIEW v_order_totals AS
  SELECT o.id AS order_id, o.customer_id, o.order_date, SUM(oi.qty * oi.price) AS items_total
  FROM shop.orders o JOIN shop.order_item oi ON oi.order_id = o.id GROUP BY o.id, o.customer_id, o.order_date;
CREATE VIEW v_product_stock AS
  SELECT p.id AS product_id, p.sku, COALESCE(SUM(i.qty), 0) AS on_hand
  FROM product p LEFT JOIN inventory i ON i.product_id = p.id GROUP BY p.id, p.sku;
CREATE VIEW v_low_stock AS SELECT * FROM shop.v_product_stock WHERE on_hand < 10;
CREATE VIEW v_customer_spend AS
  SELECT t.customer_id, COUNT(*) AS orders, SUM(t.items_total) AS spend
  FROM shop.v_order_totals t GROUP BY t.customer_id;
CREATE VIEW v_prices_in AS
  SELECT p.id, p.sku, cur.code, ROUND(p.price / cur.rate, 2) AS price
  FROM shop.product p CROSS JOIN ref.currency cur;

DELIMITER //
CREATE FUNCTION fn_on_hand(p INT) RETURNS INT READS SQL DATA
  RETURN (SELECT COALESCE(SUM(qty), 0) FROM shop.inventory WHERE product_id = p)//
CREATE FUNCTION fn_order_total(o BIGINT) RETURNS DECIMAL(12,2) READS SQL DATA
  RETURN (SELECT COALESCE(SUM(qty * price), 0) FROM order_item WHERE order_id = o)//

-- Places an order: inventory, order rows; triggers write stock_movement and audit_log.
CREATE PROCEDURE sp_place_order(IN p_customer INT, IN p_product INT, IN p_qty INT, IN p_token VARCHAR(48),
                                OUT p_order BIGINT)
BEGIN
  DECLARE v_price DECIMAL(10,2);
  SELECT price INTO v_price FROM shop.product WHERE id = p_product;
  SELECT COALESCE(MAX(id), 0) + 1 INTO p_order FROM shop.orders FOR UPDATE;
  INSERT INTO shop.orders (id, customer_id, order_date, status, total, token)
    VALUES (p_order, p_customer, CURRENT_DATE, 'new', v_price * p_qty, p_token);
  INSERT INTO order_item (order_id, line, product_id, qty, price) VALUES (p_order, 1, p_product, p_qty, v_price);
  UPDATE shop.inventory SET qty = qty - p_qty WHERE product_id = p_product AND warehouse_id = 1;
END//

-- Transfers money with dynamic SQL built from a literal (remapped at run time).
CREATE PROCEDURE sp_transfer(IN p_from INT, IN p_to INT, IN p_amount INT, IN p_token VARCHAR(48))
BEGIN
  -- (No EXECUTE ... USING: stock 9.7.4 debug builds assert on it inside a
  -- stored program when statement logging is off.)
  SET @wt_from = p_from, @wt_to = p_to, @wt_amount = p_amount;
  PREPARE s1 FROM 'UPDATE shop.account SET balance = balance - @wt_amount, version = version + 1 WHERE id = @wt_from';
  EXECUTE s1;
  DEALLOCATE PREPARE s1;
  SET @q = CONCAT('UPDATE ', 'shop', '.account SET balance = balance + @wt_amount WHERE id = @wt_to');
  PREPARE s2 FROM @q;
  EXECUTE s2;
  DEALLOCATE PREPARE s2;
  INSERT INTO shop.ledger (token, from_acct, to_acct, amount) VALUES (p_token, p_from, p_to, p_amount);
END//

CREATE PROCEDURE sp_restock(IN p_product INT, IN p_qty INT)
  UPDATE inventory SET qty = qty + p_qty WHERE product_id = p_product AND warehouse_id = 1//

-- Reports: counts rows of a table chosen at run time.
CREATE PROCEDURE sp_count(IN p_table VARCHAR(64), OUT p_n BIGINT)
BEGIN
  SET @q = CONCAT('SELECT COUNT(*) INTO @n FROM shop.', p_table);
  PREPARE s FROM @q;
  EXECUTE s;
  DEALLOCATE PREPARE s;
  SET p_n = @n;
END//

CREATE TRIGGER trg_order_item_ai AFTER INSERT ON order_item FOR EACH ROW
BEGIN
  INSERT INTO shop.stock_movement (product_id, warehouse_id, delta, reason, at)
    VALUES (NEW.product_id, 1, -NEW.qty, 'order', NOW());
  INSERT INTO shop.audit_log (entity, entity_id, action, at) VALUES ('order_item', NEW.order_id, 'insert', NOW());
END//
CREATE TRIGGER trg_orders_ai AFTER INSERT ON orders FOR EACH ROW
  INSERT INTO audit_log (entity, entity_id, action, token, at) VALUES ('order', NEW.id, 'insert', NEW.token, NOW())//
CREATE TRIGGER trg_inventory_bu BEFORE UPDATE ON inventory FOR EACH ROW
BEGIN
  IF NEW.qty < -1000000 THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'inventory underflow';
  END IF;
END//
CREATE TRIGGER trg_ledger_ai AFTER INSERT ON ledger FOR EACH ROW
  INSERT INTO shop.audit_log (entity, entity_id, action, token, at) VALUES ('ledger', NEW.id, 'insert', NEW.token, NOW())//
CREATE TRIGGER trg_review_bi BEFORE INSERT ON review FOR EACH ROW
  SET NEW.title = TRIM(NEW.title)//
CREATE TRIGGER trg_product_au AFTER UPDATE ON product FOR EACH ROW
BEGIN
  IF NEW.price <> OLD.price THEN
    INSERT INTO shop.price_history (product_id, price, changed) VALUES (NEW.id, NEW.price, NOW());
  END IF;
END//
DELIMITER ;

-- Created disabled: golden must never be written to by its own jobs.
CREATE EVENT ev_expire_sessions ON SCHEDULE EVERY 1 HOUR DISABLE
  DO DELETE FROM shop.session WHERE last_seen < NOW() - INTERVAL 1 DAY;
CREATE EVENT ev_expire_carts ON SCHEDULE EVERY 1 DAY DISABLE
  DO DELETE FROM cart WHERE created < NOW() - INTERVAL 30 DAY;
