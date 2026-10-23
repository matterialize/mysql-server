-- Fact rows for orders @lo..@hi (inclusive): 3 items, 1 payment, 2 stock
-- movements, 1 audit row and every other order a shipment. Call repeatedly
-- with consecutive ranges to build a large dataset in bounded transactions.
USE shop;
SET SESSION cte_max_recursion_depth = 10000000;
INSERT INTO orders (id, customer_id, order_date, status, total)
  WITH RECURSIVE s(n) AS (SELECT @lo UNION ALL SELECT n + 1 FROM s WHERE n < @hi)
  SELECT n, 1 + n % 2000, '2023-01-01' + INTERVAL (n % 1400) DAY, ELT(1 + n % 4, 'new', 'paid', 'shipped', 'cancelled'), 0
  FROM s;
INSERT INTO order_item (order_id, line, product_id, qty, price)
  SELECT o.id, l.n, 1 + (o.id * l.n) % 5000, 1 + o.id % 3, 1 + (o.id * 7 + l.n) % 500
  FROM orders o JOIN (SELECT 1 AS n UNION ALL SELECT 2 UNION ALL SELECT 3) l
  WHERE o.id BETWEEN @lo AND @hi;
INSERT INTO payment (id, order_id, method, amount, paid)
  SELECT o.id, o.id, 1 + o.id % 5, 10 + o.id % 990, o.order_date FROM orders o WHERE o.id BETWEEN @lo AND @hi;
INSERT INTO stock_movement (product_id, warehouse_id, delta, reason, at)
  SELECT oi.product_id, 1 + oi.order_id % 10, -oi.qty, 'order', '2023-01-01' + INTERVAL (oi.order_id % 1400) DAY
  FROM order_item oi WHERE oi.order_id BETWEEN @lo AND @hi AND oi.line <= 2;
INSERT INTO audit_log (entity, entity_id, action, at)
  SELECT 'order', o.id, 'insert', o.order_date FROM orders o WHERE o.id BETWEEN @lo AND @hi;
INSERT INTO shipment (order_id, carrier, shipped)
  SELECT o.id, ELT(1 + o.id % 3, 'UPS', 'DHL', 'FedEx'), o.order_date + INTERVAL 2 DAY FROM orders o
  WHERE o.id BETWEEN @lo AND @hi AND o.id % 2 = 0;
