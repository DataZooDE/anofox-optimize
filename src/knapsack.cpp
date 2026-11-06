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

//! Take items in descending value-per-weight while they fit. The
//! textbook heuristic; optimal for the FRACTIONAL problem, and can be
//! arbitrarily bad for 0/1 (one heavy high-ratio item can crowd out a
//! better combination).
Selection GreedyRatio(const vector<double> &values, const vector<double> &weights,
                      double capacity) {
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
Selection GreedyValue(const vector<double> &values, const vector<double> &weights,
                      double capacity) {
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

//! The setting that bounds the DP's MEMORY, separately from its cell count:
//! the table keeps one double per capacity unit whatever the item count, so
//! a single item at a capacity of 5e7 units passes the cell limit and still
//! needs ~400 MB.
constexpr const char *MAX_MEMORY_SETTING = "anofox_optimize_knapsack_max_memory";
constexpr const char *MAX_MEMORY_DEFAULT = "512MB";

//! How the exact DP sees an instance: weights and capacity as integers in
//! units of 1/scale, and whether that table fits. The ONE place the exact
//! member's limits and rounding live: knapsack_exact raises when `fits` is
//! false, knapsack_best_of skips the DP instead.
struct ExactPlan {
	double scale = 1.0;
	double cells = 0;
	double bytes = 0;
	bool within_cells = true;
	bool within_memory = true;
	//! No item weighs at most the capacity: the answer is the empty selection
	//! and no table is built, whatever the capacity.
	bool nothing_fits = false;
	bool fits() const {
		return within_cells && within_memory;
	}
	//! Only filled when `fits()` and not `nothing_fits`. An item heavier than
	//! the capacity gets `cap + 1` units: it can never be taken, and its
	//! weight is never cast to an integer (1e300 would not fit one).
	vector<idx_t> w;
	idx_t cap = 0;
};

//! Whether `x` is a whole number up to representation noise (0.3 * 10 is
//! 3.0000000000000004), and which. A few ulps RELATIVE to |x|: an absolute
//! tolerance would snap 5e-10 to 0, or 6.9999999995 up to 7.
bool NearWhole(double x, double &whole) {
	whole = std::round(x);
	return std::fabs(x - whole) <= 4 * std::numeric_limits<double>::epsilon() * std::max(1.0, std::fabs(x));
}

//! `x` (finite, >= 0, at most a table's capacity + 1) as table units: the
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
	// Items heavier than the capacity can never be taken: they play no part
	// in the scale, and never reach an integer cast.
	vector<bool> can_fit(weights.size());
	plan.nothing_fits = true;
	for (idx_t i = 0; i < weights.size(); i++) {
		can_fit[i] = weights[i] <= capacity;
		plan.nothing_fits = plan.nothing_fits && !can_fit[i];
	}
	if (plan.nothing_fits) {
		return plan;
	}
	// Scale to integers for the DP table: the smallest power of ten (up to
	// 10^4) that makes every weight that can fit, and the capacity, whole.
	// Finer weights stay at 10^4 and are rounded up, so the answer is
	// feasible but optimal only for the rounded-up weights.
	double whole;
	for (int p = 0; p < 4; p++) {
		bool integral = NearWhole(capacity * plan.scale, whole);
		for (idx_t i = 0; integral && i < weights.size(); i++) {
			integral = !can_fit[i] || NearWhole(weights[i] * plan.scale, whole);
		}
		if (integral) {
			break;
		}
		plan.scale *= 10.0;
	}
	// The real allocation: (cap + 1) doubles, plus one bit per cell in
	// per-item bit vectors (whole 64-bit words, plus each vector's header).
	const double n = static_cast<double>(weights.size());
	const double columns = capacity * plan.scale + 1;
	plan.cells = n * columns;
	plan.bytes = columns * sizeof(double) + n * (std::ceil(columns / 64) * 8 + sizeof(vector<bool>));
	plan.within_cells = plan.cells <= static_cast<double>(MAX_DP_CELLS);
	plan.within_memory = plan.bytes <= static_cast<double>(max_memory);
	if (plan.fits()) {
		// capacity * scale <= MAX_DP_CELLS here, and every weight that can fit
		// is at most the capacity, so each cast below is in range.
		plan.cap = ToUnits(capacity * plan.scale, false);
		plan.w.reserve(weights.size());
		for (idx_t i = 0; i < weights.size(); i++) {
			if (!can_fit[i]) {
				plan.w.push_back(plan.cap + 1);
				continue;
			}
			auto units = ToUnits(weights[i] * plan.scale, true);
			if (units == 0 && weights[i] > 0) {
				units = 1; // a positive weight never rides for free
			}
			plan.w.push_back(units);
		}
	}
	return plan;
}

//! Exact 0/1 knapsack by dynamic programming over the plan's integer weights.
Selection ExactDP(const vector<double> &values, const vector<double> &weights, const ExactPlan &plan) {
	const idx_t n = values.size();
	if (plan.nothing_fits) {
		return Evaluate(values, weights, vector<bool>(n, false));
	}
	const idx_t cap = plan.cap;
	const auto &w = plan.w;

	vector<double> best(cap + 1, 0.0);
	vector<vector<bool>> take(n, vector<bool>(cap + 1, false));
	for (idx_t i = 0; i < n; i++) {
		for (idx_t c = cap + 1; c-- > 0;) {
			if (w[i] <= c) {
				const double with_item = best[c - w[i]] + values[i];
				if (with_item > best[c]) {
					best[c] = with_item;
					take[i][c] = true;
				}
			}
		}
	}
	vector<bool> taken(n, false);
	idx_t c = cap;
	for (idx_t i = n; i-- > 0;) {
		if (take[i][c]) {
			taken[i] = true;
			c -= w[i];
		}
	}
	return Evaluate(values, weights, std::move(taken));
}

//! True when `taken` IS the optimum of the fractional (LP) relaxation of the
//! DP's own instance, so no 0/1 selection, the DP's included, can beat it.
//! Dantzig: an item with value <= 0 is never in the LP optimum, nor is one
//! too heavy for the table; one with positive value and zero weight always
//! is; the rest fill the capacity by descending value/weight until the
//! first that does not fit, which the LP takes FRACTIONALLY. The check is
//! combinatorial, on integer weights, not a comparison of a greedy total
//! against a floating-point bound: it passes only when that fractional part
//! is empty (everything fits, or the prefix fills the capacity exactly) and
//! `taken` is exactly that prefix. A tolerance on totals could only ever
//! skip a DP that would have won, so there is none. Value-0 items are
//! ignored; a negative one taken fails.
bool IsLpOptimal(const vector<double> &values, const ExactPlan &plan, const vector<bool> &taken) {
	if (plan.nothing_fits) {
		return true;
	}
	vector<idx_t> order;
	for (idx_t i = 0; i < values.size(); i++) {
		if (plan.w[i] > plan.cap) {
			// Too heavy for the table, so not in the LP optimum. A greedy member
			// working on the true weights may still have taken it (it can
			// round up past the capacity); then greedy is not that optimum.
			if (taken[i] && values[i] != 0) {
				return false;
			}
			continue;
		}
		if (values[i] < 0 && taken[i]) {
			return false;
		}
		if (values[i] > 0) {
			if (plan.w[i] == 0) {
				if (!taken[i]) {
					return false;
				}
			} else {
				order.push_back(i);
			}
		}
	}
	std::stable_sort(order.begin(), order.end(), [&](idx_t a, idx_t b) {
		return values[a] / static_cast<double>(plan.w[a]) > values[b] / static_cast<double>(plan.w[b]);
	});
	idx_t load = 0;
	bool full = false;
	for (auto i : order) {
		const bool in_lp = !full && load + plan.w[i] <= plan.cap;
		if (!full && !in_lp) {
			if (load < plan.cap) {
				return false; // the LP takes part of item i: greedy may be beaten
			}
			full = true;
		}
		if (in_lp) {
			load += plan.w[i];
		}
		if (taken[i] != in_lp) {
			return false;
		}
	}
	return true;
}

//! The best of the greedy members and, when its table fits both limits, the
//! exact DP, skipped when the best greedy answer already is the LP optimum.
//! The DP's answer needs no capacity re-check: weights are rounded UP into
//! the table and items heavier than the capacity are never in it, so it is
//! within capacity by construction. (Its total_weight can still print a
//! last-digit over for decimal weights — 0.1 + 0.2 is 0.30000000000000004
//! — and refusing that would throw away the optimum.)
Selection BestOfKnapsack(const vector<double> &values, const vector<double> &weights, double capacity,
                         idx_t max_memory) {
	auto best = GreedyRatio(values, weights, capacity);
	auto by_value = GreedyValue(values, weights, capacity);
	if (by_value.total_value > best.total_value) {
		best = by_value;
	}
	const auto plan = PlanExact(weights, capacity, max_memory);
	if (plan.fits() && !IsLpOptimal(values, plan, best.taken)) {
		auto exact = ExactDP(values, weights, plan);
		if (exact.total_value > best.total_value) {
			best = std::move(exact);
		}
	}
	return best;
}

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
	if (!text.empty() && text[0] != '-' && text != "none" && text != "null") {
		try {
			bytes = duckdb_compat::MemoryLimitBytes(text);
		} catch (std::exception &) {
			bytes = DConstants::INVALID_INDEX;
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

//! The session's budget, read per call so a SET takes effect immediately.
idx_t CurrentMaxMemory(ClientContext &context, string &as_set) {
	Value value;
	if (duckdb_compat::TryGetContextSetting(context, MAX_MEMORY_SETTING, value) && !value.IsNull()) {
		as_set = value.ToString();
	} else {
		as_set = MAX_MEMORY_DEFAULT;
	}
	return ParseMaxMemory(Value(as_set));
}

Selection GreedyRatioMember(const vector<double> &values, const vector<double> &weights, double capacity, idx_t) {
	return GreedyRatio(values, weights, capacity);
}

Selection GreedyValueMember(const vector<double> &values, const vector<double> &weights, double capacity, idx_t) {
	return GreedyValue(values, weights, capacity);
}

} // namespace

static void AddKnapsackFunction(ExtensionLoader &loader, const string &name,
                                Selection (*fn)(const vector<double> &, const vector<double> &,
                                                double, idx_t),
                                bool exact, const string &description, const string &example, const string &alias_of) {
	auto return_type = LogicalType::STRUCT({{"selected", LogicalType::LIST(LogicalType::BOOLEAN)},
	                                        {"total_value", LogicalType::DOUBLE},
	                                        {"total_weight", LogicalType::DOUBLE}});

	ScalarFunction function(
	    duckdb_compat::FunctionName(name),
	    {LogicalType::LIST(LogicalType::DOUBLE), LogicalType::LIST(LogicalType::DOUBLE),
	     LogicalType::DOUBLE},
	    return_type, [fn, exact, name](DataChunk &args, ExpressionState &state, Vector &result) {
#ifdef ANOFOX_TELEMETRY_ENABLED
		    PostHogTelemetry::Instance().RecordFunctionCall(name);
#endif
		    const idx_t count = args.size();
		    result.SetVectorType(VectorType::FLAT_VECTOR);
		    string max_memory_as_set;
		    const idx_t max_memory = CurrentMaxMemory(state.GetContext(), max_memory_as_set);

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

			    Selection sel;
			    if (exact) {
				    // Refuse rather than silently degrade: a caller who asked for the
				    // exact answer and got a heuristic one cannot tell it is no longer a bound.
				    const auto plan = PlanExact(weights, capacity, max_memory);
				    if (!plan.within_cells) {
					    throw InvalidInputException(
					        "exact knapsack needs a %.0f-cell table for %llu items at "
					        "capacity %f (weights scaled by %.0f to make them integral), "
					        "over the %llu-cell limit. Use opt_knapsack_greedy_ratio for "
					        "an approximate answer, or round the weights to coarser units.",
					        plan.cells, static_cast<uint64_t>(values.size()), capacity, plan.scale,
					        static_cast<uint64_t>(MAX_DP_CELLS));
				    }
				    if (!plan.within_memory) {
					    throw InvalidInputException(
					        "exact knapsack needs %s for its table for %llu items at capacity %f "
					        "(weights scaled by %.0f to make them integral), over %s = '%s'. "
					        "SET %s to a larger size, or use opt_knapsack_greedy_ratio or "
					        "opt_knapsack_best_of, which fall back to a greedy answer.",
					        StringUtil::BytesToHumanReadableString(static_cast<idx_t>(plan.bytes)),
					        static_cast<uint64_t>(values.size()), capacity, plan.scale, MAX_MEMORY_SETTING,
					        max_memory_as_set, MAX_MEMORY_SETTING);
				    }
				    sel = ExactDP(values, weights, plan);
			    } else {
				    sel = fn(values, weights, capacity, max_memory);
			    }

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

static void AddKnapsackFamily(ExtensionLoader &loader, const string &short_name,
                              Selection (*fn)(const vector<double> &, const vector<double> &,
                                              double, idx_t),
                              bool exact, const string &description, const string &example) {
	const string canonical = "anofox_optimize_" + short_name;
	AddKnapsackFunction(loader, canonical, fn, exact, description, "anofox_optimize_" + example, "");
	AddKnapsackFunction(loader, "opt_" + short_name, fn, exact, description, "opt_" + example,
	                    canonical);
}

void RegisterKnapsackFunctions(ExtensionLoader &loader) {
	DBConfig::GetConfig(loader.GetDatabaseInstance())
	    .AddExtensionOption(MAX_MEMORY_SETTING,
	                        "Largest table, in bytes, that knapsack_exact and knapsack_best_of build for their "
	                        "dynamic program (e.g. '512MB', '2GB'). Over it, knapsack_exact raises and "
	                        "knapsack_best_of returns its best greedy answer.",
	                        LogicalType::VARCHAR, Value(MAX_MEMORY_DEFAULT), OnMaxMemory);
	AddKnapsackFamily(
	    loader, "knapsack_greedy_ratio", GreedyRatioMember, false,
	    "0/1 knapsack by descending value-per-weight: takes each item that still fits. "
	    "Optimal for the FRACTIONAL problem and usually good for 0/1, but can be "
	    "arbitrarily bad — one heavy high-ratio item can crowd out a better "
	    "combination. Returns a boolean per input item plus the selected total value "
	    "and weight.",
	    "knapsack_greedy_ratio([10.0, 6.0], [5.0, 4.0], 6.0)");
	AddKnapsackFamily(
	    loader, "knapsack_greedy_value", GreedyValueMember, false,
	    "0/1 knapsack by descending VALUE, ignoring weight: takes each item that still "
	    "fits. The weakest member of the family — it loses to by-ratio whenever value "
	    "and weight are correlated — and included so a search has a poor-but-valid "
	    "option to move away from. Same signature as the other knapsack functions.",
	    "knapsack_greedy_value([10.0, 6.0], [5.0, 4.0], 6.0)");
	AddKnapsackFamily(
	    loader, "knapsack_exact", GreedyRatioMember, true,
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
	    loader, "knapsack_best_of", BestOfKnapsack, false,
	    "Runs both greedy knapsack members and, when knapsack_exact would accept the "
	    "instance, the exact dynamic program too, and returns whichever captured the "
	    "most value. knapsack_exact accepts it when its table — items * (capacity + 1) "
	    "cells, counting weights in the smallest unit (1, 0.1, ... 0.0001) that makes "
	    "every item that can fit, and the capacity, whole — is at most 50 million "
	    "cells and within the memory budget anofox_optimize_knapsack_max_memory "
	    "(default '512MB'; 8 bytes per capacity unit plus one bit per cell). There the "
	    "result is the proven optimum when every weight and the capacity are whole "
	    "multiples of 0.0001; finer weights are rounded up, and the result is then "
	    "always within capacity but optimal only for the rounded-up weights. Over "
	    "either limit best_of does NOT raise but silently returns the best greedy "
	    "answer, which is NOT exact and carries no guarantee — call knapsack_exact if "
	    "you need to know which case you are in. The DP's time and memory grow with "
	    "items * capacity, so near the limits it is far slower than the greedy "
	    "members, and knapsack_by, whose default algorithm is best_of, pays that per "
	    "group. The DP is skipped when greedy provably is the optimum: every "
	    "positive-value item fits, nothing fits, or the best-ratio prefix fills the "
	    "capacity exactly. Never loses to any single member. Same signature as the "
	    "other knapsack functions.",
	    "knapsack_best_of([10.0, 6.0], [5.0, 4.0], 6.0)");
}

} // namespace duckdb
