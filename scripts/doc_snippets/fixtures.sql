-- Fixed tables the ```sql examples in README.md, docs/ and the Claude Code skills run against
-- (scripts/check_doc_snippets.py). Deterministic literals only. A table grows here when an
-- example needs it; a fixture must never paper over a real documentation bug (an unknown
-- argument, a wrong column, a binder error).
--
-- The scheduling instance is pilot P6 from test/sql/pilot_scenarios.test: 10 jobs, asymmetric
-- setups, Held-Karp optimum 60 computed outside this extension, EDD 617.

-- cartons(carton_id, size): docs/guides/01-getting-started.md, the README's first example.
CREATE TABLE cartons AS
SELECT * FROM (VALUES (1, 448.0), (2, 460.0), (3, 450.0),
                      (4, 337.0), (5, 340.0), (6, 403.0)) AS t(carton_id, size);

-- shipments(shipment_id, weight, value): 550 + 450 fills the 1000 capacity exactly.
CREATE TABLE shipments AS
SELECT * FROM (VALUES (1, 550.0, 600.0), (2, 450.0, 520.0), (3, 300.0, 310.0),
                      (4, 250.0, 300.0), (5, 200.0, 190.0), (6, 120.0, 150.0)) AS t(shipment_id, weight, value);

-- candidates(item_id, value, weight)
CREATE TABLE candidates AS SELECT shipment_id AS item_id, value, weight FROM shipments;

-- items(item_id, warehouse_id, size): two warehouses.
CREATE TABLE items AS
SELECT i AS item_id, 1 + (i % 2) AS warehouse_id, 100.0 + ((i * 137) % 400) AS size
FROM range(1, 21) t(i);

-- ordered_items(pos, item_id, size)
CREATE TABLE ordered_items AS
SELECT row_number() OVER (ORDER BY item_id) AS pos, item_id, size FROM items;

-- p6: the pilot scheduling instance as arrays.
CREATE TABLE p6 AS SELECT
  [4.0,6.0,5.0,6.0,4.0,6.0,5.0,3.0,3.0,3.0]::DOUBLE[] AS proc,
  [26.0,41.0,38.0,22.0,12.0,61.0,28.0,18.0,45.0,24.0]::DOUBLE[] AS due,
  [2.0,8.0,1.0,5.0,8.0,2.0,8.0,2.0,2.0,1.0]::DOUBLE[] AS prio,
  [0.0,1.0,1.0,4.0,6.0,5.0,4.0,2.0,5.0,0.0,3.0,0.0,0.0,0.0,1.0,7.0,5.0,6.0,4.0,8.0,8.0,1.0,0.0,7.0,0.0,4.0,0.0,0.0,2.0,9.0,6.0,0.0,5.0,0.0,1.0,9.0,0.0,5.0,3.0,3.0,0.0,0.0,3.0,9.0,0.0,0.0,1.0,5.0,0.0,2.0,3.0,4.0,4.0,6.0,5.0,0.0,0.0,8.0,6.0,6.0,0.0,6.0,9.0,0.0,6.0,9.0,0.0,7.0,2.0,0.0,8.0,0.0,0.0,5.0,5.0,8.0,7.0,0.0,3.0,0.0,6.0,8.0,1.0,6.0,0.0,6.0,8.0,5.0,0.0,2.0,0.0,4.0,8.0,0.0,8.0,7.0,0.0,2.0,6.0,0.0,4.0,7.0,3.0,1.0,2.0,4.0,4.0,7.0,0.0,4.0,0.0,1.0,7.0,7.0,4.0,3.0,7.0,7.0,5.0,6.0,0.0]::DOUBLE[] AS setup,
  10::BIGINT AS n;

-- jobs(job, minutes, due, priority): P6 as rows, job ids 1..10.
CREATE TABLE jobs AS
SELECT j AS job, p6.proc[j] AS minutes, p6.due[j] AS due, p6.prio[j] AS priority
FROM p6, range(1, 11) t(j);

-- schedule_setups(from_job, to_job, cost): P6's (n+1)^2 matrix as triples, 0 = virtual start.
CREATE TABLE schedule_setups AS
SELECT i AS from_job, j AS to_job, p6.setup[i * 11 + j + 1] AS cost
FROM p6, range(0, 11) a(i), range(1, 11) b(j)
WHERE i <> j;

-- setup_costs(from_job, to_job, cost): job-to-job changeovers, ids 1..10, no virtual start.
CREATE TABLE setup_costs AS SELECT * FROM schedule_setups WHERE from_job > 0;

-- changeovers(from_id, to_id, value): the pairs table schedule_by takes; from_id NULL = first job.
CREATE TABLE changeovers AS
SELECT CASE WHEN from_job = 0 THEN NULL ELSE from_job END AS from_id, to_job AS to_id, cost AS value
FROM schedule_setups;

-- jobs_input / fixture: the scheduling arrays under the names the guides use (p, d, w, s, n).
-- fixture also carries a knapsack instance (v, w, cap) on which greedy-by-ratio is not optimal:
-- weights are P6's priorities, so w serves both.
CREATE TABLE jobs_input AS SELECT proc AS p, due AS d, prio AS w, setup AS s, n FROM p6;
CREATE TABLE fixture AS
SELECT proc AS p, due AS d, prio AS w, setup AS s, n,
       [5.0,15.0,1.0,9.0,14.0,3.0,13.0,4.0,3.0,1.0]::DOUBLE[] AS v, 20.0 AS cap
FROM p6;

-- inputs: one row with every family's arrays, under the names docs/guides/04 uses.
CREATE TABLE inputs AS
SELECT p, d, w, s, n,
       [0.10, 0.08, 0.12, 0.07, 0.09, 0.11]::DOUBLE[] AS mu,
       [0.040,0.006,0.010,0.002,0.004,0.008,
        0.006,0.030,0.004,0.003,0.005,0.002,
        0.010,0.004,0.060,0.006,0.007,0.012,
        0.002,0.003,0.006,0.020,0.002,0.003,
        0.004,0.005,0.007,0.002,0.035,0.006,
        0.008,0.002,0.012,0.003,0.006,0.050]::DOUBLE[] AS cov,
       (SELECT list(size ORDER BY item_id) FROM items) AS sizes,
       1000.0 AS cap
FROM jobs_input;
