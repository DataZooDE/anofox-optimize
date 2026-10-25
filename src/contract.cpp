// contract.cpp — the anofox integration contract
//
// anofox-visualization plots any table that follows the contract's long-form
// schemas (anofox-visualization docs/plans/integration-contract.md, v1). This
// module adds optimize's producers and the contract's discovery surface:
//
//   pack_by(data, item_col, size_col, capacity [, group_col, algorithms])        -> assignment
//   wave_by(data, item_col, size_col, priority_col, capacity [, group_col, algorithms])
//                                                                                -> assignment
//   schedule_by(data, job_col, processing_col, due_col, priority_col
//               [, setup_data, group_col, algorithms])                           -> schedule
//   knapsack_by(data, item_col, value_col, weight_col, capacity [, …])           (no schema yet)
//   sequence_by(data, item_col, setup_data [, …])                                (no schema yet)
//   assortment_by / assortment_recapture_by(data, item_col, margin_col, demand_col,
//                                           substitution_data, shelf [, …])      (no schema yet)
//   portfolio_by(data, item_col, return_col, covariance_data, max_holdings, weight_cap [, …])
//                                                                                (no schema yet)
//   <family>_objectives_by (the same arguments as <family>_by)                   -> summary
//   anofox_contract_version()                                                    -> '1'
//
// The producers are table macros over the caller's rows, each as anofox_optimize_* and opt_*. `data` is a
// table name or a '(SELECT …)' string and the column arguments are column names, quoted before they are
// interpolated into the query() text. Each group's rows become the solver's lists in item order, every
// algorithm named in `algorithms` solves them (model_id = the algorithm), and the answer comes back one row
// per item. A matrix (setups, substitution, covariance) comes as a pairs table (from_id, to_id, value). The
// solvers themselves stay as they are: a family keeps one signature.
//
// Discovery: every function this extension registers is tagged anofox.family = 'optimize' in
// duckdb_functions(); the producers of a contract schema also carry anofox.contract = '1' and
// anofox.output = <schema>.
//
// Plan: docs/ANOFOX_CONTRACT_PLAN.md.

#include "contract.hpp"
#include "table_macro.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb_compat.hpp"

#include <string>
#include <vector>

namespace duckdb {

namespace {

constexpr const char *ANOFOX_CONTRACT_VERSION = "1";

constexpr CatalogType FUNCTION_CATALOG_TYPES[] = {
    CatalogType::SCALAR_FUNCTION_ENTRY, CatalogType::AGGREGATE_FUNCTION_ENTRY, CatalogType::TABLE_FUNCTION_ENTRY,
    CatalogType::MACRO_ENTRY, CatalogType::TABLE_MACRO_ENTRY};

// A catalog entry's name: a string member before DuckDB 2.0, GetName() from 2.0 on.
template <class T>
auto EntryName(T &x, int) -> decltype(x.GetName()) {
	return x.GetName();
}
template <class T>
auto EntryName(T &x, long) -> decltype((x.name)) {
	return x.name;
}
template <class T>
std::string NameOf(T &x) {
	return static_cast<const std::string &>(EntryName(x, 0));
}

SchemaCatalogEntry &SystemMainSchema(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	return Catalog::GetSystemCatalog(db).GetSchema(CatalogTransaction::GetSystemTransaction(db), DEFAULT_SCHEMA);
}

// ── query() text ─────────────────────────────────────────────────────────────

// The SQL that double-quotes the identifier in macro parameter `param`.
std::string Q(const std::string &param) {
	return "'\"' || replace(" + param + ", '\"', '\"\"') || '\"'";
}

// Dyn(sql): the SQL expression that builds `sql` as query() text. Plain SQL in, with placeholders:
//   {data}         the data argument as given (a table name or a '(SELECT …)' string)
//   {pairs:param}  a pairs table argument as given, or an empty (from_id, to_id, value) relation
//   {q:param}      the column named by the parameter, double-quoted
//   {n:param}      the parameter as a DOUBLE literal
//   {i:param}      the parameter as a BIGINT literal
//   {gin}          the group column as an input expression, NULL without group_col
//   {gout}         "_g AS <group column>, " in an output select list, nothing without group_col
//   {res}          the schedule's resource: the group as text, else 'machine'
//   {a}            the algorithm (inside the per-algorithm part only)
// Single quotes in `sql` are doubled; the template itself must not contain braces.
std::string DynExpr(const std::string &token) {
	if (token == "data") {
		return "data";
	}
	if (token == "gin") {
		return "CASE WHEN group_col IS NULL THEN 'NULL' ELSE " + Q("group_col") + " END";
	}
	if (token == "gout") {
		return "CASE WHEN group_col IS NULL THEN '' ELSE '_g AS ' || " + Q("group_col") + " || ', ' END";
	}
	if (token == "res") {
		return "CASE WHEN group_col IS NULL THEN '''machine''' ELSE '_g::VARCHAR' END";
	}
	if (token == "a") {
		return "_a";
	}
	const auto colon = token.find(':');
	const auto kind = token.substr(0, colon);
	const auto param = token.substr(colon + 1);
	if (kind == "pairs") {
		return "coalesce(" + param + ", '(SELECT NULL AS from_id, NULL AS to_id, NULL::DOUBLE AS value WHERE false)')";
	}
	if (kind == "q") {
		return Q(param);
	}
	if (kind == "n") {
		return "coalesce('CAST(''' || CAST(" + param + " AS DOUBLE)::VARCHAR || ''' AS DOUBLE)', 'NULL::DOUBLE')";
	}
	if (kind == "i") {
		return "coalesce(CAST(" + param + " AS BIGINT)::VARCHAR, 'NULL::BIGINT')";
	}
	throw InternalException("anofox_optimize contract: unknown placeholder {%s}", token);
}

std::string Dyn(const std::string &sql) {
	std::string out = "'";
	for (size_t i = 0; i < sql.size(); i++) {
		if (sql[i] == '{') {
			const auto end = sql.find('}', i);
			out += "' || " + DynExpr(sql.substr(i + 1, end - i - 1)) + " || '";
			i = end;
		} else {
			out += sql[i] == '\'' ? std::string("''") : std::string(1, sql[i]);
		}
	}
	return out + "'";
}

// ── the rows and the matrix ──────────────────────────────────────────────────

// How a family's pairs table becomes the solver's row-major matrix.
enum class Pairs {
	NONE,
	// (n+1)² with index 0 the virtual start: from_id NULL is the setup before the first job (schedule).
	WITH_START,
	// n²: from_id must not be NULL (sequence, substitution).
	PLAIN,
	// n², symmetric: a pair given once counts both ways; given both ways, the values must agree (covariance).
	SYMMETRIC,
};

struct Rows {
	const char *key_param; // the id column's parameter, e.g. item_col
	const char *noun;      // what one row is, for the errors: item, job
	bool unique;           // an error when an id repeats in a group
	vector<std::pair<const char *, const char *>> numbers; // parameter -> the list it becomes in _s
	Pairs pairs;
	const char *pairs_param; // setup_data, substitution_data, covariance_data
};

// The CTEs up to `_sol AS (`:
//   _ix  the rows with the group _g, the id _key, each number column and _i, the row's 1-based place in its
//        group in id order (an error on a repeated id when `unique`);
//   _s   per group: _n rows, the ids _keys and one list per number column, all in _i order;
//   _mx  per group: the solver's matrix _m from the pairs table, 0 where a pair is missing. Pairs whose ids
//        are not in the group drop out, so one pairs table can serve every group.
std::string Prefix(const std::string &fn, const Rows &r) {
	std::string cols, lists;
	for (auto &n : r.numbers) {
		cols += std::string(", CAST({q:") + n.first + "} AS DOUBLE) AS " + n.second;
		lists += std::string(", list(") + n.second + " ORDER BY _i) AS " + n.second;
	}
	std::string number_names;
	for (auto &n : r.numbers) {
		number_names += std::string(", ") + n.second;
	}
	const std::string place = "row_number() OVER (PARTITION BY _g ORDER BY _key)";
	std::string sql = std::string("WITH _in AS (SELECT {gin} AS _g, {q:") + r.key_param + "} AS _key" + cols +
	                  " FROM (FROM {data})), _ix AS (SELECT _g, _key" + number_names + ", ";
	if (r.unique) {
		sql += std::string("CASE WHEN count(*) OVER (PARTITION BY _g, _key) > 1 THEN error('") + fn + ": " + r.noun +
		       " ''' || _key::VARCHAR || ''' appears more than once' || CASE WHEN _g IS NULL THEN '' "
		       "ELSE ' in group ''' || _g::VARCHAR || '''' END || '. Fix: give every " +
		       r.noun + " its own id, or pass group_col so each group is its own instance.') ELSE " + place + " END";
	} else {
		sql += place;
	}
	sql += " AS _i FROM _in), _s AS (SELECT _g, count(*) AS _n, list(_key ORDER BY _i) AS _keys" + lists +
	       " FROM _ix GROUP BY _g), ";
	if (r.pairs == Pairs::NONE) {
		return sql + "_sol AS (";
	}

	const std::string p = r.pairs_param;
	const std::string dup = std::string("error('") + fn +
	                        ": the pair from ''' || coalesce(from_id::VARCHAR, 'the start') || ''' to ''' || "
	                        "to_id::VARCHAR || ''' appears more than once in " +
	                        p + ". Fix: keep one row per (from_id, to_id).')";
	if (r.pairs == Pairs::SYMMETRIC) {
		// Both directions of every pair; a pair given both ways with different values is an error.
		sql += "_p0 AS (SELECT from_id AS _f, to_id AS _t, CAST(value AS DOUBLE) AS _v FROM (FROM {pairs:" + p +
		       "}) UNION ALL SELECT to_id, from_id, CAST(value AS DOUBLE) FROM (FROM {pairs:" + p +
		       "}) WHERE from_id <> to_id), "
		       "_st AS (SELECT _f, _t, CASE WHEN count(DISTINCT _v) > 1 THEN error('" +
		       fn +
		       ": the covariance of ''' || least(_f::VARCHAR, _t::VARCHAR) || ''' and ''' || "
		       "greatest(_f::VARCHAR, _t::VARCHAR) || ''' is given twice with different values in " +
		       p + ". Fix: give each pair once, or the same value both ways.') ELSE any_value(_v) END AS _v "
		           "FROM _p0 GROUP BY _f, _t), ";
	} else {
		const std::string null_from =
		    r.pairs == Pairs::PLAIN
		        ? std::string("WHEN from_id IS NULL THEN error('") + fn + ": " + p +
		              " has a NULL from_id. Fix: name the " + r.noun + " every pair starts from.') "
		        : std::string();
		sql += "_st AS (SELECT from_id AS _f, to_id AS _t, CASE " + null_from +
		       "WHEN count(*) OVER (PARTITION BY from_id, to_id) > 1 THEN " + dup +
		       " ELSE CAST(value AS DOUBLE) END AS _v FROM (FROM {pairs:" + p + "})), ";
	}
	const bool start = r.pairs == Pairs::WITH_START;
	sql += std::string("_sx AS (SELECT t._g, ") + (start ? "coalesce(f._i, 0)" : "f._i") +
	       " AS _fi, t._i AS _ti, _st._v FROM _st JOIN _ix t ON _st._t = t._key " +
	       (start ? "LEFT JOIN _ix f ON f._g IS NOT DISTINCT FROM t._g AND _st._f = f._key "
	              "WHERE _st._f IS NULL OR f._i IS NOT NULL), "
	              : "JOIN _ix f ON f._g IS NOT DISTINCT FROM t._g AND _st._f = f._key), ");
	const std::string lo = start ? "0" : "1";
	sql += "_mx AS (SELECT c._g, list(coalesce(x._v, 0) ORDER BY c._fi, c._ti) AS _m "
	       "FROM (SELECT _g, a._fi, b._ti FROM _s, range(" +
	       lo + ", _n + 1) a(_fi), range(" + lo +
	       ", _n + 1) b(_ti)) c LEFT JOIN _sx x ON x._g IS NOT DISTINCT FROM c._g AND x._fi = c._fi AND x._ti = c._ti "
	       "GROUP BY c._g), _sol AS (";
	return sql;
}

// The objectives: one summary row per (group, algorithm, metric) from `metrics` (names) and `values` (DOUBLE
// expressions over `from`, a relation with _g and model_id).
std::string Objectives(const std::string &names, const std::string &values, const std::string &from) {
	return "SELECT {gout}model_id, unnest([" + names + "]) AS metric, unnest([" + values +
	       "]) AS value, NULL::DOUBLE AS conf_low, NULL::DOUBLE AS conf_high FROM " + from +
	       " ORDER BY _g, model_id, metric";
}

// The schedule along each solver order: job _k (1-based) at _pos, its setup from the previous job (_prev, 0
// = the start), and its end, the running sum of setups and processing times.
constexpr const char *SCHEDULE_GANTT =
    "), _o AS (SELECT _g, model_id, _keys, _ps, _dues, _ws, _m, _n, _r, (unnest(_r.\"order\") + 1)::BIGINT AS _k, "
    "generate_subscripts(_r.\"order\", 1)::BIGINT AS _pos FROM _sol), "
    "_o2 AS (SELECT *, lag(_k, 1, 0) OVER (PARTITION BY _g, model_id ORDER BY _pos) AS _prev FROM _o), "
    "_gantt AS (SELECT *, _m[_prev * (_n + 1) + _k + 1] AS _su, _ps[_k] AS _pk, "
    "sum(_m[_prev * (_n + 1) + _k + 1] + _ps[_k]) OVER (PARTITION BY _g, model_id ORDER BY _pos "
    "ROWS UNBOUNDED PRECEDING) AS _end FROM _o2) ";

// A sequence is an open path: the first item has no setup, each next one the setup from the item before.
constexpr const char *SEQUENCE_PATH =
    "), _o AS (SELECT _g, model_id, _keys, _m, _n, (unnest(_r.\"order\") + 1)::BIGINT AS _k, "
    "generate_subscripts(_r.\"order\", 1)::BIGINT AS _pos FROM _sol), "
    "_o2 AS (SELECT *, coalesce(_m[(lag(_k) OVER (PARTITION BY _g, model_id ORDER BY _pos) - 1) * _n + _k], 0) "
    "AS _su FROM _o) ";

// ── the families ─────────────────────────────────────────────────────────────

struct Family {
	const char *name; // pack, wave, schedule, …
	vector<std::string> algorithms;
	const char *const *parameters;
	const char *const *defaults;
	vector<std::string> parameter_names;
	Rows rows;
	std::string call; // the solver call over _s / _m, with {a} for the algorithm
	std::string long_tail;
	std::string objectives_tail;
	const char *output; // the long form's contract schema, nullptr while the visualization has none
	std::string long_description;
	std::string objectives_description;
	std::string long_example;
	std::string objectives_example;
	const char *category;
};

const char *const PACK_PARAMETERS[] = {"data", "item_col", "size_col", "capacity", nullptr};
const char *const WAVE_PARAMETERS[] = {"data", "item_col", "size_col", "priority_col", "capacity", nullptr};
const char *const SCHEDULE_PARAMETERS[] = {"data", "job_col", "processing_col", "due_col", "priority_col", nullptr};
const char *const KNAPSACK_PARAMETERS[] = {"data", "item_col", "value_col", "weight_col", "capacity", nullptr};
const char *const SEQUENCE_PARAMETERS[] = {"data", "item_col", "setup_data", nullptr};
const char *const ASSORTMENT_PARAMETERS[] = {"data",       "item_col",          "margin_col", "demand_col",
                                             "substitution_data", "shelf", nullptr};
const char *const PORTFOLIO_PARAMETERS[] = {"data",         "item_col",   "return_col", "covariance_data",
                                            "max_holdings", "weight_cap", nullptr};
const char *const GROUP_DEFAULTS[] = {"group_col=NULL", "algorithms=['best_of']", nullptr};
const char *const SCHEDULE_DEFAULTS[] = {"setup_data=NULL", "group_col=NULL", "algorithms=['best_of']", nullptr};

constexpr const char *GROUP_NOTE = " Each group of data is one instance, solved by every algorithm in algorithms "
                                   "(default ['best_of']); data is a table name or a '(SELECT ...)' string, the "
                                   "column arguments are column names.";

Family Assortment(const char *name, const char *model) {
	const std::string family = name;
	return {name,
	        {"best_of", "greedy_marginal", "local_search", "top_margin"},
	        ASSORTMENT_PARAMETERS,
	        GROUP_DEFAULTS,
	        {"data", "item_col", "margin_col", "demand_col", "substitution_data", "shelf", "group_col", "algorithms"},
	        {"item_col", "item", true, {{"margin_col", "_margins"}, {"demand_col", "_demands"}}, Pairs::PLAIN,
	         "substitution_data"},
	        "SELECT s.*, m._m, '{a}' AS model_id, anofox_optimize_" + family +
	            "_{a}(s._margins, s._demands, m._m, {i:shelf}) AS _r FROM _s s JOIN _mx m ON s._g IS NOT DISTINCT FROM m._g",
	        ") SELECT {gout}model_id, item, margin, demand, listed FROM (SELECT _g, model_id, unnest(_keys) AS item, "
	        "unnest(_margins) AS margin, unnest(_demands) AS demand, unnest(_r.listed) AS listed FROM _sol) "
	        "ORDER BY _g, model_id, item",
	        ") " + Objectives("'captured_margin', 'items_listed'",
	                          "_r.captured_margin, len([x FOR x IN _r.listed IF x])::DOUBLE", "_sol"),
	        nullptr,
	        "Table macro: the shelf as one row per product: the group column, model_id (the algorithm), item, "
	        "margin, demand, listed. " +
	            std::string(model) +
	            " substitution_data is a table or '(SELECT ...)' with columns from_id, to_id, value: the fraction of "
	            "from_id's demand that moves to to_id." + GROUP_NOTE,
	        "Table macro: the objectives of " + family +
	            "_* per group and algorithm (the anofox integration contract's `summary` schema): the group column, "
	            "model_id (the algorithm), metric (captured_margin, items_listed), value, conf_low, conf_high. Same "
	            "arguments as " +
	            family + "_by.",
	        "SELECT * FROM opt_" + family +
	            "_by('products', 'sku', 'margin', 'demand', 'substitution', 24, algorithms := ['top_margin', "
	            "'best_of']);",
	        "SELECT * FROM opt_" + family + "_objectives_by('products', 'sku', 'margin', 'demand', 'substitution', 24);",
	        "selection"};
}

vector<Family> Families() {
	vector<Family> families = {
	    {"pack",
	     {"best_fit_decreasing", "best_of", "bin_completion", "first_fit_decreasing", "local_search", "next_fit",
	      "worst_fit_decreasing"},
	     PACK_PARAMETERS,
	     GROUP_DEFAULTS,
	     {"data", "item_col", "size_col", "capacity", "group_col", "algorithms"},
	     {"item_col", "item", false, {{"size_col", "_sizes"}}, Pairs::NONE, nullptr},
	     "SELECT *, '{a}' AS model_id, anofox_optimize_pack_{a}(_sizes, {n:capacity}) AS _r FROM _s",
	     ") SELECT {gout}model_id, bin, item, size, capacity FROM (SELECT _g, model_id, "
	     "(unnest(_r.assignment) + 1)::BIGINT AS bin, unnest(_keys) AS item, unnest(_sizes) AS size, "
	     "{n:capacity} AS capacity FROM _sol) ORDER BY _g, model_id, bin, item",
	     ") " + Objectives("'bins_used'", "_r.bins_used::DOUBLE", "_sol"),
	     "assignment",
	     "Table macro: bin packing as one row per item (the anofox integration contract's `assignment` schema, for "
	     "anofox_plot): the group column, model_id (the algorithm), bin (1-based), item, size, capacity. Each group "
	     "of data is one instance, packed by every pack_* algorithm in algorithms (default ['best_of']). data is a "
	     "table name or a '(SELECT ...)' string; the column arguments are column names.",
	     "Table macro: the objective of bin packing per group and algorithm (the anofox integration contract's "
	     "`summary` schema): the group column, model_id (the algorithm), metric (bins_used), value, conf_low, "
	     "conf_high. Same arguments as pack_by.",
	     "SELECT * FROM opt_pack_by('cartons', 'carton', 'size', 10.0, algorithms := ['first_fit_decreasing', "
	     "'bin_completion']);",
	     "SELECT * FROM opt_pack_objectives_by('cartons', 'carton', 'size', 10.0, group_col := 'site');",
	     "packing"},
	    {"wave",
	     {"as_given", "best_of", "fewest_waves", "priority_density", "priority_first"},
	     WAVE_PARAMETERS,
	     GROUP_DEFAULTS,
	     {"data", "item_col", "size_col", "priority_col", "capacity", "group_col", "algorithms"},
	     {"item_col", "item", false, {{"size_col", "_sizes"}, {"priority_col", "_prios"}}, Pairs::NONE, nullptr},
	     "SELECT *, '{a}' AS model_id, anofox_optimize_wave_{a}(_sizes, _prios, {n:capacity}) AS _r FROM _s",
	     ") SELECT {gout}model_id, wave, item, size, capacity, priority FROM (SELECT _g, model_id, "
	     "unnest(_r.wave)::BIGINT AS wave, unnest(_keys) AS item, unnest(_sizes) AS size, {n:capacity} AS capacity, "
	     "unnest(_prios) AS priority FROM _sol) ORDER BY _g, model_id, wave, item",
	     ") " + Objectives("'avg_weighted_wave', 'waves_used'",
	                       "_r.avg_weighted_wave, coalesce(list_max(_r.wave), 0)::DOUBLE", "_sol"),
	     "assignment",
	     "Table macro: pick-wave batching as one row per order (the anofox integration contract's `assignment` "
	     "schema, for anofox_plot): the group column, model_id (the algorithm), wave (1-based), item, size, "
	     "capacity, priority. Each group of data is one instance, batched by every wave_* algorithm in algorithms "
	     "(default ['best_of']). data is a table name or a '(SELECT ...)' string; the column arguments are column "
	     "names.",
	     "Table macro: the objectives of pick-wave batching per group and algorithm (the anofox integration "
	     "contract's `summary` schema): the group column, model_id (the algorithm), metric (avg_weighted_wave, "
	     "waves_used), value, conf_low, conf_high. Same arguments as wave_by.",
	     "SELECT * FROM opt_wave_by('orders', 'order_id', 'lines', 'priority', 40.0);",
	     "SELECT * FROM opt_wave_objectives_by('orders', 'order_id', 'lines', 'priority', 40.0, "
	     "algorithms := ['as_given', 'best_of']);",
	     "batching"},
	    {"schedule",
	     {"atcs", "best_of", "edd", "exact", "local_search", "wspt"},
	     SCHEDULE_PARAMETERS,
	     SCHEDULE_DEFAULTS,
	     {"data", "job_col", "processing_col", "due_col", "priority_col", "setup_data", "group_col", "algorithms"},
	     {"job_col",
	      "job",
	      true,
	      {{"processing_col", "_ps"}, {"due_col", "_dues"}, {"priority_col", "_ws"}},
	      Pairs::WITH_START,
	      "setup_data"},
	     "SELECT s.*, m._m, '{a}' AS model_id, anofox_optimize_schedule_{a}(s._ps, s._dues, s._ws, m._m, s._n) AS _r "
	     "FROM _s s JOIN _mx m ON s._g IS NOT DISTINCT FROM m._g",
	     std::string(SCHEDULE_GANTT) +
	         "SELECT {gout}model_id, {res} AS resource, _keys[_k] AS task, _pos AS position, _end - _pk AS start, "
	         "_end AS \"end\", _su AS setup, _pk AS processing, _dues[_k] AS due, _ws[_k] AS priority, "
	         "greatest(_end - _dues[_k], 0) AS tardiness, CASE WHEN _end > _dues[_k] THEN 'late' ELSE 'on time' END "
	         "AS status FROM _gantt ORDER BY _g, model_id, _pos",
	     std::string(SCHEDULE_GANTT) +
	         Objectives("'late_jobs', 'makespan', 'total_weighted_tardiness'", "_late, _mk, _twt",
	                    "(SELECT _g, model_id, count(*) FILTER (WHERE _end > _dues[_k])::DOUBLE AS _late, "
	                    "any_value(_r.makespan) AS _mk, any_value(_r.total_weighted_tardiness) AS _twt FROM _gantt "
	                    "GROUP BY _g, model_id)"),
	     "schedule",
	     "Table macro: single-machine scheduling as one row per job, a Gantt chart (the anofox integration "
	     "contract's `schedule` schema, for anofox_plot): the group column, model_id (the algorithm), resource (the "
	     "group, else 'machine'), task (the job), position, start, end, setup, processing, due, priority, tardiness, "
	     "status ('on time' / 'late'). A job's setup runs before it, so the last end is the makespan. Each group of "
	     "data is one machine, scheduled by every schedule_* algorithm in algorithms (default ['best_of']). "
	     "setup_data is a table or '(SELECT ...)' with columns from_id, to_id, value (from_id NULL: before the "
	     "first job); a missing pair costs 0. Job ids must be unique per group.",
	     "Table macro: the objectives of single-machine scheduling per group and algorithm (the anofox integration "
	     "contract's `summary` schema): the group column, model_id (the algorithm), metric (late_jobs, makespan, "
	     "total_weighted_tardiness), value, conf_low, conf_high. Same arguments as schedule_by.",
	     "SELECT * FROM opt_schedule_by('jobs', 'job', 'minutes', 'due', 'priority', setup_data := 'changeovers', "
	     "algorithms := ['edd', 'best_of']);",
	     "SELECT * FROM opt_schedule_objectives_by('jobs', 'job', 'minutes', 'due', 'priority', "
	     "setup_data := 'changeovers');",
	     "scheduling"},
	    {"knapsack",
	     {"best_of", "exact", "greedy_ratio", "greedy_value"},
	     KNAPSACK_PARAMETERS,
	     GROUP_DEFAULTS,
	     {"data", "item_col", "value_col", "weight_col", "capacity", "group_col", "algorithms"},
	     {"item_col", "item", true, {{"value_col", "_vals"}, {"weight_col", "_wts"}}, Pairs::NONE, nullptr},
	     "SELECT *, '{a}' AS model_id, anofox_optimize_knapsack_{a}(_vals, _wts, {n:capacity}) AS _r FROM _s",
	     ") SELECT {gout}model_id, item, value, weight, selected, capacity FROM (SELECT _g, model_id, "
	     "unnest(_keys) AS item, unnest(_vals) AS value, unnest(_wts) AS weight, unnest(_r.selected) AS selected, "
	     "{n:capacity} AS capacity FROM _sol) ORDER BY _g, model_id, item",
	     ") " + Objectives("'items_selected', 'total_value', 'total_weight'",
	                       "len([x FOR x IN _r.selected IF x])::DOUBLE, _r.total_value, _r.total_weight", "_sol"),
	     nullptr,
	     "Table macro: the knapsack as one row per item: the group column, model_id (the algorithm), item, value, "
	     "weight, selected, capacity." +
	         std::string(GROUP_NOTE),
	     "Table macro: the objectives of the knapsack per group and algorithm (the anofox integration contract's "
	     "`summary` schema): the group column, model_id (the algorithm), metric (items_selected, total_value, "
	     "total_weight), value, conf_low, conf_high. Same arguments as knapsack_by.",
	     "SELECT * FROM opt_knapsack_by('shipments', 'id', 'value', 'kg', 20.0, algorithms := ['greedy_value', "
	     "'exact']);",
	     "SELECT * FROM opt_knapsack_objectives_by('shipments', 'id', 'value', 'kg', 20.0);",
	     "knapsack"},
	    {"sequence",
	     {"as_given", "best_of", "nearest_neighbour", "two_opt"},
	     SEQUENCE_PARAMETERS,
	     GROUP_DEFAULTS,
	     {"data", "item_col", "setup_data", "group_col", "algorithms"},
	     {"item_col", "item", true, {}, Pairs::PLAIN, "setup_data"},
	     "SELECT s.*, m._m, '{a}' AS model_id, anofox_optimize_sequence_{a}(m._m, s._n) AS _r "
	     "FROM _s s JOIN _mx m ON s._g IS NOT DISTINCT FROM m._g",
	     std::string(SEQUENCE_PATH) +
	         "SELECT {gout}model_id, _pos AS position, _keys[_k] AS item, _su AS setup, sum(_su) OVER (PARTITION BY _g, "
	         "model_id ORDER BY _pos ROWS UNBOUNDED PRECEDING) AS cumulative_setup FROM _o2 ORDER BY _g, model_id, _pos",
	     ") " + Objectives("'total_setup'", "_r.total_setup", "_sol"),
	     nullptr,
	     "Table macro: a sequence as one row per item in run order: the group column, model_id (the algorithm), "
	     "position, item, setup (from the item before; 0 for the first), cumulative_setup. setup_data is a table or "
	     "'(SELECT ...)' with columns from_id, to_id, value: the cost of running to_id right after from_id; a "
	     "missing pair costs 0." +
	         std::string(GROUP_NOTE),
	     "Table macro: the objective of sequencing per group and algorithm (the anofox integration contract's "
	     "`summary` schema): the group column, model_id (the algorithm), metric (total_setup), value, conf_low, "
	     "conf_high. Same arguments as sequence_by.",
	     "SELECT * FROM opt_sequence_by('stops', 'stop', 'distances', algorithms := ['nearest_neighbour', "
	     "'two_opt']);",
	     "SELECT * FROM opt_sequence_objectives_by('stops', 'stop', 'distances');",
	     "sequencing"},
	    {"portfolio",
	     {"best_of", "greedy_sharpe", "top_return", "top_return_optimized"},
	     PORTFOLIO_PARAMETERS,
	     GROUP_DEFAULTS,
	     {"data", "item_col", "return_col", "covariance_data", "max_holdings", "weight_cap", "group_col",
	      "algorithms"},
	     {"item_col", "asset", true, {{"return_col", "_rets"}}, Pairs::SYMMETRIC, "covariance_data"},
	     "SELECT s.*, m._m, '{a}' AS model_id, anofox_optimize_portfolio_{a}(s._rets, m._m, {i:max_holdings}, "
	     "{n:weight_cap}) AS _r FROM _s s JOIN _mx m ON s._g IS NOT DISTINCT FROM m._g",
	     ") SELECT {gout}model_id, item, expected_return, weight FROM (SELECT _g, model_id, unnest(_keys) AS item, "
	     "unnest(_rets) AS expected_return, unnest(_r.weights) AS weight FROM _sol) ORDER BY _g, model_id, item",
	     ") " + Objectives("'expected_return', 'holdings', 'sharpe', 'volatility'",
	                       "_r.expected_return, len([x FOR x IN _r.weights IF x > 0])::DOUBLE, _r.sharpe, "
	                       "_r.volatility",
	                       "_sol"),
	     nullptr,
	     "Table macro: a portfolio as one row per asset: the group column, model_id (the algorithm), item, "
	     "expected_return, weight. covariance_data is a table or '(SELECT ...)' with columns from_id, to_id, value; "
	     "a pair given once counts both ways." +
	         std::string(GROUP_NOTE),
	     "Table macro: the objectives of a portfolio per group and algorithm (the anofox integration contract's "
	     "`summary` schema): the group column, model_id (the algorithm), metric (expected_return, holdings, sharpe, "
	     "volatility), value, conf_low, conf_high. Same arguments as portfolio_by.",
	     "SELECT * FROM opt_portfolio_by('assets', 'ticker', 'expected_return', 'covariance', 5, 0.3);",
	     "SELECT * FROM opt_portfolio_objectives_by('assets', 'ticker', 'expected_return', 'covariance', 5, 0.3, "
	     "algorithms := ['top_return', 'best_of']);",
	     "portfolio"},
	};
	families.push_back(Assortment("assortment",
	                              "MODEL: CANNIBALISATION (as assortment_*): a listed product loses the demand that "
	                              "moves to the other listed products."));
	families.push_back(Assortment("assortment_recapture",
	                              "MODEL: RECAPTURE (as assortment_recapture_*): a listed product keeps its demand "
	                              "and gains what moves to it from delisted products."));
	return families;
}

// The algorithms argument, checked against the family's members: an error names the first unknown one.
std::string CheckedAlgorithms(const std::string &fn, const Family &f) {
	std::string valid = "[", listed;
	for (auto &a : f.algorithms) {
		valid += (valid.size() > 1 ? ", '" : "'") + a + "'";
		listed += (listed.empty() ? "" : ", ") + a;
	}
	valid += "]";
	return "CASE WHEN algorithms IS NULL OR len(algorithms) = 0 THEN error('" + fn +
	       ": algorithms is empty. Fix: pass a list of " + f.name + "_* algorithms: " + listed +
	       ".') WHEN len([x FOR x IN algorithms IF NOT list_contains(" + valid + ", x)]) > 0 THEN error('" + fn +
	       ": unknown algorithm ''' || [x FOR x IN algorithms IF NOT list_contains(" + valid +
	       ", x)][1] || '''. Fix: pass a list of " + f.name + "_* algorithms: " + listed +
	       ".') ELSE algorithms::VARCHAR[] END";
}

std::string Body(const std::string &fn, const Family &f, const std::string &tail) {
	return "SELECT * FROM query(" + Dyn(Prefix(fn, f.rows)) + " || array_to_string([" + Dyn(f.call) + " FOR _a IN " +
	       CheckedAlgorithms(fn, f) + "], ' UNION ALL ') || " + Dyn(tail) + ")";
}

unique_ptr<CreateMacroInfo> TableMacro(const std::string &name, const std::string &alias_of, const Family &f,
                                       const std::string &tail, const std::string &description,
                                       const std::string &example) {
	const auto body = Body(name, f, tail);
	auto info = anofox_optimize::MakeTableMacroInfo(name, f.parameters, f.defaults, body.c_str());
	info->temporary = true;
	info->internal = true;
	info->alias_of = duckdb_compat::Ident(alias_of);

	FunctionDescription fd;
	fd.parameter_names = f.parameter_names;
	fd.description = description;
	fd.examples = {example};
	fd.categories = {"optimize", f.category};
	info->descriptions.push_back(std::move(fd));
	return info;
}

void ContractVersionFunction(DataChunk &, ExpressionState &, Vector &result) {
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	result.SetValue(0, Value(ANOFOX_CONTRACT_VERSION));
}

} // namespace

case_insensitive_set_t SnapshotSystemFunctionNames(ExtensionLoader &loader) {
	case_insensitive_set_t names;
	auto &schema = SystemMainSchema(loader);
	for (auto type : FUNCTION_CATALOG_TYPES) {
		schema.Scan(type, [&](CatalogEntry &entry) { names.insert(NameOf(entry)); });
	}
	return names;
}

void RegisterContractFunctions(ExtensionLoader &loader) {
	// Shared by the whole anofox family: whichever extension loads first registers it (a second identical
	// overload would make this load fail).
	if (!loader.TryGetFunction("anofox_contract_version")) {
		ScalarFunction func("anofox_contract_version", {}, LogicalType::VARCHAR, ContractVersionFunction);
		CreateScalarFunctionInfo info(func);
		FunctionDescription desc;
		desc.description = "Returns the version of the anofox integration contract (output schemas and "
		                   "duckdb_functions() tags) this extension implements.";
		desc.examples = {"SELECT anofox_contract_version();"};
		desc.categories = {"metadata"};
		info.descriptions.push_back(std::move(desc));
		loader.RegisterFunction(std::move(info));
	}

	for (auto &f : Families()) {
		for (auto &kind : {std::string("_by"), std::string("_objectives_by")}) {
			const bool is_long = kind == "_by";
			const std::string suffix = std::string(f.name) + kind;
			const std::string primary = "anofox_optimize_" + suffix;
			const auto &tail = is_long ? f.long_tail : f.objectives_tail;
			const auto &description = is_long ? f.long_description : f.objectives_description;
			const auto &example = is_long ? f.long_example : f.objectives_example;
			loader.RegisterFunction(*TableMacro(primary, std::string(), f, tail, description, example));
			loader.RegisterFunction(*TableMacro("opt_" + suffix, primary, f, tail, description, example));
		}
	}
}

void ApplyContractTags(ExtensionLoader &loader, const case_insensitive_set_t &preexisting) {
	case_insensitive_map_t<std::string> outputs;
	for (auto &f : Families()) {
		for (auto &prefix : {"anofox_optimize_", "opt_"}) {
			if (f.output) {
				outputs[std::string(prefix) + f.name + "_by"] = f.output;
			}
			outputs[std::string(prefix) + f.name + "_objectives_by"] = "summary";
		}
	}
	auto &schema = SystemMainSchema(loader);
	for (auto type : FUNCTION_CATALOG_TYPES) {
		schema.Scan(type, [&](CatalogEntry &entry) {
			auto name = NameOf(entry);
			if (preexisting.find(name) != preexisting.end() || name == "anofox_contract_version") {
				return;
			}
			entry.tags["anofox.family"] = "optimize";
			auto output = outputs.find(name);
			if (output != outputs.end()) {
				entry.tags["anofox.contract"] = ANOFOX_CONTRACT_VERSION;
				entry.tags["anofox.output"] = output->second;
			}
		});
	}
}

} // namespace duckdb
