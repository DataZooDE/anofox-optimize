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
//   pack_objectives_by / wave_objectives_by / schedule_objectives_by (same arguments)
//                                                                                -> summary
//   anofox_contract_version()                                                    -> '1'
//
// The producers are table macros over the caller's rows, each as anofox_optimize_* and opt_*. `data` is a
// table name or a '(SELECT …)' string and the column arguments are column names, quoted before they are
// interpolated into the query() text. Each group's rows become the solver's lists in item order, every
// algorithm named in `algorithms` solves them (model_id = the algorithm), and the answer comes back one row
// per item. The solvers themselves stay as they are: a family keeps one signature.
//
// Discovery: every function this extension registers is tagged anofox.family = 'optimize' in
// duckdb_functions(); the producers also carry anofox.contract = '1' and anofox.output = <schema>.
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
//   {data}      the data argument as given (a table name or a '(SELECT …)' string)
//   {setup}     setup_data as given, or an empty (from_job, to_job, setup) relation
//   {q:param}   the column named by the parameter, double-quoted
//   {n:param}   the parameter as a DOUBLE literal
//   {gin}       the group column as an input expression, NULL without group_col
//   {gout}      "_g AS <group column>, " in an output select list, nothing without group_col
//   {res}       the schedule's resource: the group as text, else 'machine'
//   {a}         the algorithm (inside the per-algorithm part only)
// Single quotes in `sql` are doubled; the template itself must not contain braces.
std::string DynExpr(const std::string &token) {
	if (token == "data") {
		return "data";
	}
	if (token == "setup") {
		return "coalesce(setup_data, '(SELECT NULL AS from_job, NULL AS to_job, NULL::DOUBLE AS setup WHERE false)')";
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
	const auto param = token.substr(2);
	if (token.rfind("q:", 0) == 0) {
		return Q(param);
	}
	if (token.rfind("n:", 0) == 0) {
		return "coalesce('CAST(''' || CAST(" + param + " AS DOUBLE)::VARCHAR || ''' AS DOUBLE)', 'NULL::DOUBLE')";
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

// ── the families ─────────────────────────────────────────────────────────────

struct Family {
	const char *name; // pack, wave, schedule
	vector<std::string> algorithms;
	const char *const *parameters;
	const char *const *defaults;
	vector<std::string> parameter_names;
	// The CTEs up to `_sol AS (`: the rows (_in), the solver's lists per group (_s) and anything else the
	// solver call needs. `fn` is the function name for the error messages.
	std::string (*prefix)(const std::string &fn);
	// One algorithm's solve over _s: SELECT … , '{a}' AS model_id, <solver>(…) AS _r.
	const char *solve;
	// After `)`: the long form, and the objectives.
	const char *long_tail;
	const char *objectives_tail;
	const char *output; // the long form's contract schema
	const char *long_description;
	const char *objectives_description;
	const char *long_example;
	const char *objectives_example;
	const char *category;
};

std::string PackPrefix(const std::string &) {
	return "WITH _in AS (SELECT {gin} AS _g, {q:item_col} AS _item, CAST({q:size_col} AS DOUBLE) AS _size "
	       "FROM (FROM {data})), "
	       "_s AS (SELECT _g, list(_item ORDER BY _item) AS _items, list(_size ORDER BY _item) AS _sizes "
	       "FROM _in GROUP BY _g), "
	       "_sol AS (";
}

std::string WavePrefix(const std::string &) {
	return "WITH _in AS (SELECT {gin} AS _g, {q:item_col} AS _item, CAST({q:size_col} AS DOUBLE) AS _size, "
	       "CAST({q:priority_col} AS DOUBLE) AS _prio FROM (FROM {data})), "
	       "_s AS (SELECT _g, list(_item ORDER BY _item) AS _items, list(_size ORDER BY _item) AS _sizes, "
	       "list(_prio ORDER BY _item) AS _prios FROM _in GROUP BY _g), "
	       "_sol AS (";
}

// _ix numbers each group's jobs 1..n in job order (an error on a repeated job). _sx maps setup_data onto
// those numbers, 0 being the virtual start; pairs whose jobs are not in a group drop out. _mx is the solver's
// (n+1)² row-major setup matrix, 0 where setup_data has no pair.
std::string SchedulePrefix(const std::string &fn) {
	return "WITH _in AS (SELECT {gin} AS _g, {q:job_col} AS _job, CAST({q:processing_col} AS DOUBLE) AS _p, "
	       "CAST({q:due_col} AS DOUBLE) AS _due, CAST({q:priority_col} AS DOUBLE) AS _w FROM (FROM {data})), "
	       "_ix AS (SELECT _g, _job, _p, _due, _w, CASE WHEN count(*) OVER (PARTITION BY _g, _job) > 1 THEN error('" +
	       fn +
	       ": job ''' || _job::VARCHAR || ''' appears more than once' || CASE WHEN _g IS NULL THEN '' "
	       "ELSE ' in group ''' || _g::VARCHAR || '''' END || '. Fix: give every job its own id, or pass "
	       "group_col so each group is its own schedule.') ELSE row_number() OVER (PARTITION BY _g ORDER BY _job) "
	       "END AS _i FROM _in), "
	       "_st AS (SELECT from_job AS _f, to_job AS _t, CASE WHEN count(*) OVER (PARTITION BY from_job, to_job) > 1 "
	       "THEN error('" +
	       fn +
	       ": the setup from ''' || coalesce(from_job::VARCHAR, 'the start') || ''' to ''' || to_job::VARCHAR || "
	       "''' appears more than once in setup_data. Fix: keep one row per (from_job, to_job).') "
	       "ELSE CAST(setup AS DOUBLE) END AS _v FROM (FROM {setup})), "
	       "_sx AS (SELECT t._g, coalesce(f._i, 0) AS _fi, t._i AS _ti, _st._v FROM _st JOIN _ix t ON _st._t = t._job "
	       "LEFT JOIN _ix f ON f._g IS NOT DISTINCT FROM t._g AND _st._f = f._job "
	       "WHERE _st._f IS NULL OR f._i IS NOT NULL), "
	       "_s AS (SELECT _g, count(*) AS _n, list(_job ORDER BY _i) AS _jobs, list(_p ORDER BY _i) AS _ps, "
	       "list(_due ORDER BY _i) AS _dues, list(_w ORDER BY _i) AS _ws FROM _ix GROUP BY _g), "
	       "_mx AS (SELECT c._g, list(coalesce(x._v, 0) ORDER BY c._fi, c._ti) AS _setup "
	       "FROM (SELECT _g, a._fi, b._ti FROM _s, range(0, _n + 1) a(_fi), range(0, _n + 1) b(_ti)) c "
	       "LEFT JOIN _sx x ON x._g IS NOT DISTINCT FROM c._g AND x._fi = c._fi AND x._ti = c._ti GROUP BY c._g), "
	       "_sol AS (";
}

// The schedule along each solver order: job _k (1-based) at _pos, its setup from the previous job (_prev, 0
// = the start), and its end, the running sum of setups and processing times.
constexpr const char *SCHEDULE_GANTT =
    "), _o AS (SELECT _g, model_id, _jobs, _ps, _dues, _ws, _setup, _n, _r, (unnest(_r.\"order\") + 1)::BIGINT AS _k, "
    "generate_subscripts(_r.\"order\", 1)::BIGINT AS _pos FROM _sol), "
    "_o2 AS (SELECT *, lag(_k, 1, 0) OVER (PARTITION BY _g, model_id ORDER BY _pos) AS _prev FROM _o), "
    "_gantt AS (SELECT *, _setup[_prev * (_n + 1) + _k + 1] AS _su, _ps[_k] AS _pk, "
    "sum(_setup[_prev * (_n + 1) + _k + 1] + _ps[_k]) OVER (PARTITION BY _g, model_id ORDER BY _pos "
    "ROWS UNBOUNDED PRECEDING) AS _end FROM _o2) ";

const std::string SCHEDULE_LONG_TAIL =
    std::string(SCHEDULE_GANTT) +
    "SELECT {gout}model_id, {res} AS resource, _jobs[_k] AS task, _pos AS position, _end - _pk AS start, "
    "_end AS \"end\", _su AS setup, _pk AS processing, _dues[_k] AS due, _ws[_k] AS priority, "
    "greatest(_end - _dues[_k], 0) AS tardiness, CASE WHEN _end > _dues[_k] THEN 'late' ELSE 'on time' END AS status "
    "FROM _gantt ORDER BY _g, model_id, _pos";

const std::string SCHEDULE_OBJECTIVES_TAIL =
    std::string(SCHEDULE_GANTT) +
    "SELECT {gout}model_id, unnest(['late_jobs', 'makespan', 'total_weighted_tardiness']) AS metric, "
    "unnest([_late, _mk, _twt]) AS value, NULL::DOUBLE AS conf_low, NULL::DOUBLE AS conf_high "
    "FROM (SELECT _g, model_id, count(*) FILTER (WHERE _end > _dues[_k])::DOUBLE AS _late, "
    "any_value(_r.makespan) AS _mk, any_value(_r.total_weighted_tardiness) AS _twt FROM _gantt GROUP BY _g, model_id) "
    "ORDER BY _g, model_id, metric";

const char *const PACK_PARAMETERS[] = {"data", "item_col", "size_col", "capacity", nullptr};
const char *const WAVE_PARAMETERS[] = {"data", "item_col", "size_col", "priority_col", "capacity", nullptr};
const char *const SCHEDULE_PARAMETERS[] = {"data", "job_col", "processing_col", "due_col", "priority_col", nullptr};
const char *const GROUP_DEFAULTS[] = {"group_col=NULL", "algorithms=['best_of']", nullptr};
const char *const SCHEDULE_DEFAULTS[] = {"setup_data=NULL", "group_col=NULL", "algorithms=['best_of']", nullptr};

vector<Family> Families() {
	return {
	    {"pack",
	     {"best_fit_decreasing", "best_of", "bin_completion", "first_fit_decreasing", "local_search", "next_fit",
	      "worst_fit_decreasing"},
	     PACK_PARAMETERS,
	     GROUP_DEFAULTS,
	     {"data", "item_col", "size_col", "capacity", "group_col", "algorithms"},
	     PackPrefix,
	     "SELECT *, '{a}' AS model_id, anofox_optimize_pack_{a}(_sizes, {n:capacity}) AS _r FROM _s",
	     ") SELECT {gout}model_id, bin, item, size, capacity FROM (SELECT _g, model_id, "
	     "(unnest(_r.assignment) + 1)::BIGINT AS bin, unnest(_items) AS item, unnest(_sizes) AS size, "
	     "{n:capacity} AS capacity FROM _sol) ORDER BY _g, model_id, bin, item",
	     ") SELECT {gout}model_id, 'bins_used' AS metric, _r.bins_used::DOUBLE AS value, NULL::DOUBLE AS conf_low, "
	     "NULL::DOUBLE AS conf_high FROM _sol ORDER BY _g, model_id",
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
	     WavePrefix,
	     "SELECT *, '{a}' AS model_id, anofox_optimize_wave_{a}(_sizes, _prios, {n:capacity}) AS _r FROM _s",
	     ") SELECT {gout}model_id, wave, item, size, capacity, priority FROM (SELECT _g, model_id, "
	     "unnest(_r.wave)::BIGINT AS wave, unnest(_items) AS item, unnest(_sizes) AS size, {n:capacity} AS capacity, "
	     "unnest(_prios) AS priority FROM _sol) ORDER BY _g, model_id, wave, item",
	     ") SELECT {gout}model_id, unnest(['avg_weighted_wave', 'waves_used']) AS metric, "
	     "unnest([_r.avg_weighted_wave, coalesce(list_max(_r.wave), 0)::DOUBLE]) AS value, NULL::DOUBLE AS conf_low, "
	     "NULL::DOUBLE AS conf_high FROM _sol ORDER BY _g, model_id, metric",
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
	     SchedulePrefix,
	     "SELECT s.*, m._setup, '{a}' AS model_id, anofox_optimize_schedule_{a}(s._ps, s._dues, s._ws, m._setup, "
	     "s._n) AS _r FROM _s s JOIN _mx m ON s._g IS NOT DISTINCT FROM m._g",
	     nullptr,
	     nullptr,
	     "schedule",
	     "Table macro: single-machine scheduling as one row per job, a Gantt chart (the anofox integration "
	     "contract's `schedule` schema, for anofox_plot): the group column, model_id (the algorithm), resource (the "
	     "group, else 'machine'), task (the job), position, start, end, setup, processing, due, priority, tardiness, "
	     "status ('on time' / 'late'). A job's setup runs before it, so the last end is the makespan. Each group of "
	     "data is one machine, scheduled by every schedule_* algorithm in algorithms (default ['best_of']). "
	     "setup_data is a table or '(SELECT ...)' with columns from_job, to_job, setup (from_job NULL: before the "
	     "first job); a missing pair costs 0. Job ids must be unique per group.",
	     "Table macro: the objectives of single-machine scheduling per group and algorithm (the anofox integration "
	     "contract's `summary` schema): the group column, model_id (the algorithm), metric (late_jobs, makespan, "
	     "total_weighted_tardiness), value, conf_low, conf_high. Same arguments as schedule_by.",
	     "SELECT * FROM opt_schedule_by('jobs', 'job', 'minutes', 'due', 'priority', setup_data := 'changeovers', "
	     "algorithms := ['edd', 'best_of']);",
	     "SELECT * FROM opt_schedule_objectives_by('jobs', 'job', 'minutes', 'due', 'priority', "
	     "setup_data := 'changeovers');",
	     "scheduling"},
	};
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
	return "SELECT * FROM query(" + Dyn(f.prefix(fn)) + " || array_to_string([" + Dyn(f.solve) + " FOR _a IN " +
	       CheckedAlgorithms(fn, f) + "], ' UNION ALL ') || " + Dyn(tail) + ")";
}

unique_ptr<CreateMacroInfo> TableMacro(const std::string &name, const std::string &alias_of, const Family &f,
                                       const std::string &tail, const char *description, const char *example) {
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
		const std::string long_tail = f.long_tail ? f.long_tail : SCHEDULE_LONG_TAIL;
		const std::string objectives_tail = f.objectives_tail ? f.objectives_tail : SCHEDULE_OBJECTIVES_TAIL;
		for (auto &kind : {std::string("_by"), std::string("_objectives_by")}) {
			const bool is_long = kind == "_by";
			const std::string suffix = std::string(f.name) + kind;
			const std::string primary = "anofox_optimize_" + suffix;
			const auto &tail = is_long ? long_tail : objectives_tail;
			const char *description = is_long ? f.long_description : f.objectives_description;
			const char *example = is_long ? f.long_example : f.objectives_example;
			loader.RegisterFunction(*TableMacro(primary, std::string(), f, tail, description, example));
			loader.RegisterFunction(*TableMacro("opt_" + suffix, primary, f, tail, description, example));
		}
	}
}

void ApplyContractTags(ExtensionLoader &loader, const case_insensitive_set_t &preexisting) {
	case_insensitive_map_t<std::string> outputs;
	for (auto &f : Families()) {
		for (auto &prefix : {"anofox_optimize_", "opt_"}) {
			outputs[std::string(prefix) + f.name + "_by"] = f.output;
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
