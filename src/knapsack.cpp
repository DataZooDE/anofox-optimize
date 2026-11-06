#include "knapsack.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

#include "register_alias.hpp"
#include "duckdb_compat/list_vector.hpp"
#include "duckdb_compat/struct_vector.hpp"
#include "duckdb_compat.hpp"

#ifdef ANOFOX_TELEMETRY_ENABLED
#include "telemetry.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <vector>

namespace duckdb {

namespace {

struct Selection {
	vector<bool> taken;
	double total_value = 0;
	double total_weight = 0;
};

Selection Evaluate(const vector<double> &values, const vector<double> &weights,
                   vector<bool> taken) {
	Selection s;
	s.taken = std::move(taken);
	for (idx_t i = 0; i < values.size(); i++) {
		if (s.taken[i]) {
			s.total_value += values[i];
			s.total_weight += weights[i];
		}
	}
	return s;
}

//! The setting that bounds the DP's MEMORY, separately from its cell count:
//! the table keeps one double per capacity unit whatever the item count, so
//! a single item at a capacity of 5e7 units passes the cell limit and still
//! needs ~400 MB.
constexpr const char *MAX_MEMORY_SETTING = "anofox_optimize_knapsack_max_memory";
constexpr const char *MAX_MEMORY_DEFAULT = "512MB";

//! The memory budget as bytes, rejecting what is not a concrete size.
//! DuckDB's own parser reads '-1', 'none' and 'null' as "unlimited"; a
//! budget must be a size, so those are refused rather than silently
//! lifting the bound.
idx_t ParseMaxMemory(const Value &parameter) {
	if (parameter.IsNull()) {
		throw InvalidInputException("%s cannot be NULL — give a size such as '%s'", MAX_MEMORY_SETTING,
		                            MAX_MEMORY_DEFAULT);
	}
	auto text = parameter.ToString();
	StringUtil::Trim(text);
	text = StringUtil::Lower(text);
	idx_t bytes = DConstants::INVALID_INDEX;
	if (!text.empty()) {
		try {
			bytes = duckdb_compat::MemoryLimitBytes(text);
		} catch (std::exception &) {
		}
	}
	if (bytes == DConstants::INVALID_INDEX || bytes == NumericLimits<idx_t>::Maximum()) {
		throw InvalidInputException("%s must be a size such as '%s' or '2GB', got '%s'", MAX_MEMORY_SETTING,
		                            MAX_MEMORY_DEFAULT, parameter.ToString());
	}
	return bytes;
}

void OnMaxMemory(ClientContext &, SetScope, Value &parameter) {
	ParseMaxMemory(parameter);
}

//! The session's budget, read once per chunk on first use, so a SET takes
//! effect on the next call and the greedy members never parse it.
class MemoryBudget {
public:
	explicit MemoryBudget(ClientContext &context) : context(context) {
	}
	idx_t Bytes() {
		if (as_set.empty()) {
			Value value;
			if (duckdb_compat::TryGetContextSetting(context, MAX_MEMORY_SETTING, value) && !value.IsNull()) {
				as_set = value.ToString();
			} else {
				as_set = MAX_MEMORY_DEFAULT;
			}
			bytes = ParseMaxMemory(Value(as_set));
		}
		return bytes;
	}
	//! The setting as the user wrote it, for error messages; set by Bytes().
	const string &AsSet() const {
		return as_set;
	}

private:
	ClientContext &context;
	string as_set;
	idx_t bytes = 0;
};

//! Take items in descending value-per-weight while they fit. The
//! textbook heuristic; optimal for the FRACTIONAL problem, and can be
//! arbitrarily bad for 0/1 (one heavy high-ratio item can crowd out a
//! better combination).
Selection GreedyRatio(const vector<double> &values, const vector<double> &weights, double capacity,
                      MemoryBudget &) {
	vector<idx_t> order(values.size());
	std::iota(order.begin(), order.end(), 0);
	std::stable_sort(order.begin(), order.end(), [&](idx_t a, idx_t b) {
		const double ra = weights[a] > 0 ? values[a] / weights[a] : std::numeric_limits<double>::max();
		const double rb = weights[b] > 0 ? values[b] / weights[b] : std::numeric_limits<double>::max();
		return ra > rb;
	});
	vector<bool> taken(values.size(), false);
	double load = 0;
	for (auto i : order) {
		if (load + weights[i] <= capacity) {
			taken[i] = true;
			load += weights[i];
		}
	}
	return Evaluate(values, weights, std::move(taken));
}

//! Take items in descending VALUE while they fit. Deliberately weaker
//! than by-ratio: a search needs a poor-but-valid option to move away
//! from, and this one loses whenever value and weight are correlated.
Selection GreedyValue(const vector<double> &values, const vector<double> &weights, double capacity,
                      MemoryBudget &) {
	vector<idx_t> order(values.size());
	std::iota(order.begin(), order.end(), 0);
	std::stable_sort(order.begin(), order.end(),
	                 [&](idx_t a, idx_t b) { return values[a] > values[b]; });
	vector<bool> taken(values.size(), false);
	double load = 0;
	for (auto i : order) {
		if (load + weights[i] <= capacity) {
			taken[i] = true;
			load += weights[i];
		}
	}
	return Evaluate(values, weights, std::move(taken));
}

//! Largest DP table this will build, in cells (a time bound). Beyond it the
//! exact member REFUSES rather than silently degrading to a heuristic: a
//! caller who asked for the exact answer and got a greedy one, with no
//! signal, cannot tell that its result is no longer a bound. best_of shares
//! the same limit, but falls back to its greedy members instead of raising.
constexpr idx_t MAX_DP_CELLS = 50u * 1000u * 1000u;

//! How the exact DP sees an instance: the items that can fit at all, their
//! weights and the capacity in integer units of 1/scale, and whether that
//! table is within both limits. knapsack_exact raises when it is not,
//! knapsack_best_of skips the DP.
struct ExactPlan {
	double scale = 1.0;
	double cells = 0;
	double bytes = 0;
	bool within_cells = true;
	bool within_memory = true;
	bool fits() const {
		return within_cells && within_memory;
	}
	//! Items weighing at most the capacity; the rest can never be taken.
	vector<idx_t> kept;
	//! Units per kept item, and the capacity; only filled when `fits()`.
	vector<idx_t> w;
	idx_t cap = 0;
};

//! Whether `x` is a whole number up to representation noise (0.3 * 10 is
//! 3.0000000000000004), and which. A few ulps RELATIVE to |x|, so a positive
//! x never snaps to 0 and 6.9999999995 never snaps up to 7.
bool NearWhole(double x, double &whole) {
	whole = std::round(x);
	return std::fabs(x - whole) <= 4 * std::numeric_limits<double>::epsilon() * std::fabs(x);
}

//! `x` (finite, >= 0, at most a table's capacity) as table units: the
//! nearest whole number when it is one up to representation noise, else
//! rounded `up` or down. Rounding weights up and the capacity down is what
//! keeps a DP selection within the true capacity when weights are finer
//! than the scale: an item never weighs less in the table than it really
//! does.
idx_t ToUnits(double x, bool up) {
	double whole;
	if (!NearWhole(x, whole)) {
		whole = up ? std::ceil(x) : std::floor(x);
	}
	return static_cast<idx_t>(whole);
}

ExactPlan PlanExact(const vector<double> &weights, double capacity, idx_t max_memory) {
	ExactPlan plan;
	// Items heavier than the capacity play no part in the scale, and never
	// reach an integer cast (1e300 would not fit one).
	for (idx_t i = 0; i < weights.size(); i++) {
		if (weights[i] <= capacity) {
			plan.kept.push_back(i);
		}
	}
	if (plan.kept.empty()) {
		return plan; // the empty selection, whatever the capacity
	}
	// Scale to integers for the DP table: the smallest power of ten (up to
	// 10^4) that makes every kept weight, and the capacity, whole. Finer
	// weights stay at 10^4 and are rounded up, so the answer is feasible but
	// optimal only for the rounded-up weights.
	double whole;
	for (int p = 0; p < 4; p++) {
		bool integral = NearWhole(capacity * plan.scale, whole);
		for (idx_t i = 0; integral && i < plan.kept.size(); i++) {
			integral = NearWhole(weights[plan.kept[i]] * plan.scale, whole);
		}
		if (integral) {
			break;
		}
		plan.scale *= 10.0;
	}
	// Both limits count every item, kept or not, so they match the documented
	// items * (capacity + 1). The bytes bound the real allocation: (cap + 1)
	// doubles plus one bit buffer of kept * (cap + 1) bits in 64-bit words.
	const double n = static_cast<double>(weights.size());
	const double columns = capacity * plan.scale + 1;
	plan.cells = n * columns;
	// ceil: the capacity may round UP to a whole unit (2.9999999999999996 -> 3).
	const double table_columns = std::ceil(columns);
	plan.bytes = table_columns * sizeof(double) + std::ceil(n * table_columns / 64) * 8 + sizeof(vector<uint64_t>);
	plan.within_cells = plan.cells <= static_cast<double>(MAX_DP_CELLS);
	plan.within_memory = plan.bytes <= static_cast<double>(max_memory);
	if (plan.fits()) {
		// capacity * scale <= MAX_DP_CELLS here, and every kept weight is at
		// most the capacity, so each cast below is in range.
		plan.cap = ToUnits(capacity * plan.scale, false);
		plan.w.reserve(plan.kept.size());
		for (auto i : plan.kept) {
			plan.w.push_back(ToUnits(weights[i] * plan.scale, true));
		}
	}
	return plan;
}

//! Exact 0/1 knapsack by dynamic programming over the plan's integer weights.
Selection ExactDP(const vector<double> &values, const vector<double> &weights, const ExactPlan &plan) {
	vector<bool> taken(values.size(), false);
	if (plan.kept.empty()) {
		return Evaluate(values, weights, std::move(taken));
	}
	const idx_t k = plan.kept.size();
	const idx_t columns = plan.cap + 1;
	const auto &w = plan.w;

	vector<double> best(columns, 0.0);
	vector<uint64_t> take((k * columns + 63) / 64, 0);
	for (idx_t i = 0; i < k; i++) {
		const double value = values[plan.kept[i]];
		for (idx_t c = columns; c-- > w[i];) {
			const double with_item = best[c - w[i]] + value;
			if (with_item > best[c]) {
				best[c] = with_item;
				const idx_t bit = i * columns + c;
				take[bit / 64] |= uint64_t(1) << (bit % 64);
			}
		}
	}
	idx_t c = plan.cap;
	for (idx_t i = k; i-- > 0;) {
		const idx_t bit = i * columns + c;
		if (take[bit / 64] >> (bit % 64) & 1) {
			taken[plan.kept[i]] = true;
			c -= w[i];
		}
	}
	return Evaluate(values, weights, std::move(taken));
}

//! True when `taken` IS the optimum of the fractional (LP) relaxation of the
//! DP's own instance, so no 0/1 selection can beat it and the DP can be
//! skipped. Combinatorial, on the integer weights: no tolerance to tune.
bool IsLpOptimal(const vector<double> &values, const ExactPlan &plan, const vector<bool> &taken) {
	// Dantzig: value <= 0 is never in the LP optimum; positive value at zero
	// weight always is; the rest fill by descending value/weight.
	vector<idx_t> order;
	for (idx_t j = 0; j < plan.kept.size(); j++) {
		const idx_t i = plan.kept[j];
		if (values[i] < 0 && taken[i]) {
			return false;
		}
		if (values[i] > 0) {
			if (plan.w[j] == 0) {
				if (!taken[i]) {
					return false;
				}
			} else {
				order.push_back(j);
			}
		}
	}
	std::stable_sort(order.begin(), order.end(), [&](idx_t a, idx_t b) {
		return values[plan.kept[a]] / static_cast<double>(plan.w[a]) >
		       values[plan.kept[b]] / static_cast<double>(plan.w[b]);
	});
	idx_t load = 0;
	for (auto j : order) {
		const bool in_lp = load + plan.w[j] <= plan.cap;
		if (!in_lp && load < plan.cap) {
			return false; // the LP takes part of this item: greedy may be beaten
		}
		if (in_lp) {
			load += plan.w[j];
		}
		if (taken[plan.kept[j]] != in_lp) {
			return false;
		}
	}
	return true;
}

//! Exact, or a refusal naming the limit: a caller who asked for the exact
//! answer and got a heuristic one cannot tell it is no longer a bound.
Selection ExactMember(const vector<double> &values, const vector<double> &weights, double capacity,
                      MemoryBudget &budget) {
	const auto plan = PlanExact(weights, capacity, budget.Bytes());
	if (!plan.within_cells) {
		throw InvalidInputException("exact knapsack needs a %.0f-cell table for %llu items at "
		                            "capacity %f (weights scaled by %.0f to make them integral), "
		                            "over the %llu-cell limit. Use opt_knapsack_greedy_ratio for "
		                            "an approximate answer, or round the weights to coarser units.",
		                            plan.cells, static_cast<uint64_t>(values.size()), capacity, plan.scale,
		                            static_cast<uint64_t>(MAX_DP_CELLS));
	}
	if (!plan.within_memory) {
		throw InvalidInputException("exact knapsack needs %s for its table for %llu items at capacity %f "
		                            "(weights scaled by %.0f to make them integral), over %s = '%s'. "
		                            "SET %s to a larger size, or use opt_knapsack_greedy_ratio or "
		                            "opt_knapsack_best_of, which fall back to a greedy answer.",
		                            StringUtil::BytesToHumanReadableString(static_cast<idx_t>(plan.bytes)),
		                            static_cast<uint64_t>(values.size()), capacity, plan.scale,
		                            MAX_MEMORY_SETTING, budget.AsSet(), MAX_MEMORY_SETTING);
	}
	return ExactDP(values, weights, plan);
}

//! The best greedy answer, and the DP's when its table fits and greedy is
//! not already the LP optimum. The DP is within capacity by construction
//! (weights rounded up), so its answer needs no re-check.
Selection BestOfKnapsack(const vector<double> &values, const vector<double> &weights, double capacity,
                         MemoryBudget &budget) {
	auto best = GreedyRatio(values, weights, capacity, budget);
	auto by_value = GreedyValue(values, weights, capacity, budget);
	if (by_value.total_value > best.total_value) {
		best = by_value;
	}
	const auto plan = PlanExact(weights, capacity, budget.Bytes());
	if (plan.fits() && !IsLpOptimal(values, plan, best.taken)) {
		auto exact = ExactDP(values, weights, plan);
		if (exact.total_value > best.total_value) {
			best = std::move(exact);
		}
	}
	return best;
}

using KnapsackMember = Selection (*)(const vector<double> &, const vector<double> &, double, MemoryBudget &);

} // namespace

static void AddKnapsackFunction(ExtensionLoader &loader, const string &name, KnapsackMember fn,
                                const string &description, const string &example, const string &alias_of) {
	auto return_type = LogicalType::STRUCT({{"selected", LogicalType::LIST(LogicalType::BOOLEAN)},
	                                        {"total_value", LogicalType::DOUBLE},
	                                        {"total_weight", LogicalType::DOUBLE}});

	ScalarFunction function(
	    duckdb_compat::FunctionName(name),
	    {LogicalType::LIST(LogicalType::DOUBLE), LogicalType::LIST(LogicalType::DOUBLE),
	     LogicalType::DOUBLE},
	    return_type, [fn, name](DataChunk &args, ExpressionState &state, Vector &result) {
#ifdef ANOFOX_TELEMETRY_ENABLED
		    PostHogTelemetry::Instance().RecordFunctionCall(name);
#endif
		    const idx_t count = args.size();
		    result.SetVectorType(VectorType::FLAT_VECTOR);
		    MemoryBudget budget(state.GetContext());

		    UnifiedVectorFormat v_data, w_data, c_data;
		    duckdb_compat::ToUnifiedFormat(args.data[0], count, v_data);
		    duckdb_compat::ToUnifiedFormat(args.data[1], count, w_data);
		    duckdb_compat::ToUnifiedFormat(args.data[2], count, c_data);
		    auto v_lists = UnifiedVectorFormat::GetData<list_entry_t>(v_data);
		    auto w_lists = UnifiedVectorFormat::GetData<list_entry_t>(w_data);
		    auto caps = UnifiedVectorFormat::GetData<double>(c_data);

		    auto &v_child = ListVector::GetEntry(args.data[0]);
		    auto &w_child = ListVector::GetEntry(args.data[1]);
		    UnifiedVectorFormat vc, wc;
		    duckdb_compat::ToUnifiedFormat(v_child, ListVector::GetListSize(args.data[0]), vc);
		    duckdb_compat::ToUnifiedFormat(w_child, ListVector::GetListSize(args.data[1]), wc);
		    auto v_vals = UnifiedVectorFormat::GetData<double>(vc);
		    auto w_vals = UnifiedVectorFormat::GetData<double>(wc);

		    auto &entries = StructVector::GetEntries(result);
		    auto &sel_vec = duckdb_compat::StructChild(entries[0]);
		    auto sel_out = duckdb_compat::FlatData<list_entry_t>(sel_vec);
		    auto value_out = duckdb_compat::FlatData<double>(duckdb_compat::StructChild(entries[1]));
		    auto weight_out = duckdb_compat::FlatData<double>(duckdb_compat::StructChild(entries[2]));
		    idx_t sel_offset = 0;

		    for (idx_t row = 0; row < count; row++) {
			    const auto vi = v_data.sel->get_index(row);
			    const auto wi = w_data.sel->get_index(row);
			    const auto ci = c_data.sel->get_index(row);
			    if (!v_data.validity.RowIsValid(vi) || !w_data.validity.RowIsValid(wi) ||
			        !c_data.validity.RowIsValid(ci)) {
				    FlatVector::SetNull(result, row, true);
				    sel_out[row].offset = sel_offset;
				    sel_out[row].length = 0;
				    value_out[row] = 0;
				    weight_out[row] = 0;
				    continue;
			    }

			    const double capacity = caps[ci];
			    if (!std::isfinite(capacity) || capacity < 0) {
				    throw InvalidInputException(
				        "capacity must be finite and >= 0, got %f", capacity);
			    }
			    const auto ve = v_lists[vi];
			    const auto we = w_lists[wi];
			    if (ve.length != we.length) {
				    throw InvalidInputException(
				        "values and weights must be the same length, got %llu and %llu — "
				        "each item needs exactly one of each",
				        static_cast<uint64_t>(ve.length), static_cast<uint64_t>(we.length));
			    }

			    vector<double> values, weights;
			    values.reserve(ve.length);
			    weights.reserve(we.length);
			    for (idx_t i = 0; i < ve.length; i++) {
				    const auto vidx = vc.sel->get_index(ve.offset + i);
				    const auto widx = wc.sel->get_index(we.offset + i);
				    if (!vc.validity.RowIsValid(vidx) || !wc.validity.RowIsValid(widx)) {
					    throw InvalidInputException(
					        "item %llu has a NULL value or weight — a NULL cannot be "
					        "compared, so it would silently never be selected",
					        static_cast<uint64_t>(i));
				    }
				    const double val = v_vals[vidx];
				    const double wgt = w_vals[widx];
				    if (!std::isfinite(val) || !std::isfinite(wgt)) {
					    throw InvalidInputException(
					        "item %llu has value %f and weight %f — both must be finite; a "
					        "non-finite value silently defeats every comparison",
					        static_cast<uint64_t>(i), val, wgt);
				    }
				    if (wgt < 0) {
					    throw InvalidInputException(
					        "item %llu has weight %f — weights must be >= 0; a negative "
					        "weight would 'free up' capacity that does not exist",
					        static_cast<uint64_t>(i), wgt);
				    }
				    values.push_back(val);
				    weights.push_back(wgt);
			    }

			    const auto sel = fn(values, weights, capacity, budget);

			    value_out[row] = sel.total_value;
			    weight_out[row] = sel.total_weight;
			    sel_out[row].offset = sel_offset;
			    sel_out[row].length = sel.taken.size();
			    ListVector::Reserve(sel_vec, sel_offset + sel.taken.size());
			    auto sel_child = duckdb_compat::FlatData<bool>(ListVector::GetEntry(sel_vec));
			    for (idx_t i = 0; i < sel.taken.size(); i++) {
				    sel_child[sel_offset + i] = sel.taken[i];
			    }
			    sel_offset += sel.taken.size();
		    }
		    duckdb_compat::MutableValidity(ListVector::GetEntry(sel_vec)).SetAllValid(sel_offset);
		    ListVector::SetListSize(sel_vec, sel_offset);
		    FinishResult(args, result);
	    });

	RegisterScalarOrAlias(loader, std::move(function), description, example, alias_of,
	                      {"values", "weights", "capacity"}, {"optimize", "knapsack"});
}

static void AddKnapsackFamily(ExtensionLoader &loader, const string &short_name, KnapsackMember fn,
                              const string &description, const string &example) {
	const string canonical = "anofox_optimize_" + short_name;
	AddKnapsackFunction(loader, canonical, fn, description, "anofox_optimize_" + example, "");
	AddKnapsackFunction(loader, "opt_" + short_name, fn, description, "opt_" + example, canonical);
}

void RegisterKnapsackFunctions(ExtensionLoader &loader) {
	DBConfig::GetConfig(loader.GetDatabaseInstance())
	    .AddExtensionOption(MAX_MEMORY_SETTING,
	                        "Largest table, in bytes, that knapsack_exact and knapsack_best_of build for their "
	                        "dynamic program (e.g. '512MB', '2GB'). Over it, knapsack_exact raises and "
	                        "knapsack_best_of returns its best greedy answer.",
	                        LogicalType::VARCHAR, Value(MAX_MEMORY_DEFAULT), OnMaxMemory);
	AddKnapsackFamily(
	    loader, "knapsack_greedy_ratio", GreedyRatio,
	    "0/1 knapsack by descending value-per-weight: takes each item that still fits. "
	    "Optimal for the FRACTIONAL problem and usually good for 0/1, but can be "
	    "arbitrarily bad — one heavy high-ratio item can crowd out a better "
	    "combination. Returns a boolean per input item plus the selected total value "
	    "and weight.",
	    "knapsack_greedy_ratio([10.0, 6.0], [5.0, 4.0], 6.0)");
	AddKnapsackFamily(
	    loader, "knapsack_greedy_value", GreedyValue,
	    "0/1 knapsack by descending VALUE, ignoring weight: takes each item that still "
	    "fits. The weakest member of the family — it loses to by-ratio whenever value "
	    "and weight are correlated — and included so a search has a poor-but-valid "
	    "option to move away from. Same signature as the other knapsack functions.",
	    "knapsack_greedy_value([10.0, 6.0], [5.0, 4.0], 6.0)");
	AddKnapsackFamily(
	    loader, "knapsack_exact", ExactMember,
	    "0/1 knapsack solved by dynamic programming over integer-scaled weights. "
	    "Returns the proven optimum when every weight and the capacity are whole "
	    "multiples of 0.0001, unlike the greedy members. Finer weights are rounded UP "
	    "to the next 0.0001 (and the capacity down), so the answer is always within "
	    "capacity but optimal only for the rounded-up weights. Items heavier than the "
	    "capacity are never taken and do not affect the scaling. The table has "
	    "items * (capacity + 1) cells, counting weights in the smallest unit (1, 0.1, "
	    "... 0.0001) that makes every item that can fit, and the capacity, whole. It "
	    "needs 8 bytes per capacity unit plus one bit per cell. RAISES rather than "
	    "degrading when the table is over 50 million cells or over the memory budget "
	    "set by anofox_optimize_knapsack_max_memory (default '512MB'), since a caller "
	    "who asked for the exact answer and silently received a heuristic one could "
	    "not tell its result is no longer a bound. Time and memory grow with items * "
	    "capacity, so near those limits it is far slower than the greedy members. Same "
	    "signature as the other knapsack functions.",
	    "knapsack_exact([10.0, 6.0], [5.0, 4.0], 6.0)");
	AddKnapsackFamily(
	    loader, "knapsack_best_of", BestOfKnapsack,
    "Runs both greedy knapsack members and, when knapsack_exact would accept the "
    "instance (the same limits: at most 50 million table cells, within "
    "anofox_optimize_knapsack_max_memory), knapsack_exact's dynamic program too, and "
    "returns whichever captured the most value. Where the DP runs, the result is the "
    "proven optimum when every weight and the capacity are whole multiples of 0.0001; "
    "finer weights are rounded up, and the result is then always within capacity but "
    "optimal only for the rounded-up weights. Where knapsack_exact would refuse, "
    "best_of does NOT raise but silently returns the best greedy answer, which is NOT "
    "exact and carries no guarantee — call knapsack_exact if you need to know which "
    "case you are in. The DP's time and memory grow with items * capacity, so near "
    "the limits it is far slower than the greedy members, and knapsack_by, whose "
    "default algorithm is best_of, pays that per group. The DP is skipped when greedy "
    "provably is the optimum: every positive-value item fits, nothing fits, or the "
    "best-ratio prefix fills the capacity exactly. Never loses to any single member. "
    "Same signature as the other knapsack functions.",
    "knapsack_best_of([10.0, 6.0], [5.0, 4.0], 6.0)");
}

} // namespace duckdb
