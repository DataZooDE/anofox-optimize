#include "knapsack.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

#include "register_alias.hpp"
#include "duckdb_compat/list_vector.hpp"
#include "duckdb_compat/struct_vector.hpp"

#ifdef ANOFOX_TELEMETRY_ENABLED
#include "telemetry.hpp"
#endif

#include <algorithm>
#include <cmath>
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

//! Largest DP table this will build. Beyond it the exact member REFUSES
//! rather than silently degrading to a heuristic: a caller who asked for
//! the exact answer and got a greedy one, with no signal, cannot tell
//! that its result is no longer a bound. best_of shares the same limit,
//! but falls back to its greedy members instead of raising.
constexpr idx_t MAX_DP_CELLS = 50u * 1000u * 1000u;

//! How the exact DP sees an instance: weights and capacity as integers in
//! units of 1/scale, and whether that table fits. The ONE place the exact
//! member's limit and rounding live: knapsack_exact raises when `fits` is
//! false, knapsack_best_of skips the DP instead.
struct ExactPlan {
	double scale = 1.0;
	double cells = 0;
	bool fits = false;
	//! Only filled when `fits`.
	vector<idx_t> w;
	idx_t cap = 0;
};

//! `x` as an integer: to nearest when it is integral up to decimal
//! representation noise (0.3 * 10 is 3.0000000000000004), else rounded
//! `up` or down. Rounding weights up and the capacity down is what keeps a
//! DP selection within the true capacity when weights are finer than the
//! scale: an item never weighs less in the table than it really does.
idx_t ToUnits(double x, bool up) {
	const double nearest = std::round(x);
	if (std::fabs(x - nearest) <= 1e-9) {
		return static_cast<idx_t>(nearest);
	}
	return static_cast<idx_t>(up ? std::ceil(x) : std::floor(x));
}

ExactPlan PlanExact(const vector<double> &weights, double capacity) {
	// Scale to integers for the DP table: the smallest power of ten (up to
	// 10^4) that makes every weight and the capacity integral. Finer weights
	// stay at 10^4 and are rounded up, so the answer is feasible but optimal
	// only for the rounded-up weights.
	ExactPlan plan;
	for (int p = 0; p < 4; p++) {
		bool integral = true;
		for (auto wgt : weights) {
			if (std::fabs(wgt * plan.scale - std::llround(wgt * plan.scale)) > 1e-9) {
				integral = false;
				break;
			}
		}
		if (integral && std::fabs(capacity * plan.scale - std::llround(capacity * plan.scale)) <= 1e-9) {
			break;
		}
		plan.scale *= 10.0;
	}
	plan.cells = static_cast<double>(weights.size()) * (capacity * plan.scale + 1);
	plan.fits = plan.cells <= static_cast<double>(MAX_DP_CELLS);
	if (plan.fits) {
		plan.cap = ToUnits(capacity * plan.scale, false);
		plan.w.reserve(weights.size());
		for (auto wgt : weights) {
			plan.w.push_back(ToUnits(wgt * plan.scale, true));
		}
	}
	return plan;
}

//! Exact 0/1 knapsack by dynamic programming over the plan's integer weights.
Selection ExactDP(const vector<double> &values, const vector<double> &weights, const ExactPlan &plan) {
	const idx_t n = values.size();
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
//! Dantzig: an item with value <= 0 is never in the LP optimum, one with
//! positive value and zero weight always is, the rest fill the capacity by
//! descending value/weight until the first that does not fit, which the LP
//! takes FRACTIONALLY. The check is combinatorial, on integer weights, not a
//! comparison of a greedy total against a floating-point bound: it passes
//! only when that fractional part is empty (everything fits, or the prefix
//! fills the capacity exactly) and `taken` is exactly that prefix. A
//! tolerance on totals could only ever skip a DP that would have won, so
//! there is none. Value-0 items are ignored; a negative one taken fails.
bool IsLpOptimal(const vector<double> &values, const ExactPlan &plan, const vector<bool> &taken) {
	vector<idx_t> order;
	for (idx_t i = 0; i < values.size(); i++) {
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

//! The best of the greedy members and, when its table fits, the exact DP,
//! skipped when the best greedy answer already is the LP optimum. The DP's
//! answer needs no capacity re-check: weights are rounded UP into the
//! table, so it is within capacity by construction. (Its total_weight can
//! still print a last-digit over for decimal weights — 0.1 + 0.2 is
//! 0.30000000000000004 — and refusing that would throw away the optimum.)
Selection BestOfKnapsack(const vector<double> &values, const vector<double> &weights,
                         double capacity) {
	auto best = GreedyRatio(values, weights, capacity);
	auto by_value = GreedyValue(values, weights, capacity);
	if (by_value.total_value > best.total_value) {
		best = by_value;
	}
	const auto plan = PlanExact(weights, capacity);
	if (plan.fits && !IsLpOptimal(values, plan, best.taken)) {
		auto exact = ExactDP(values, weights, plan);
		if (exact.total_value > best.total_value) {
			best = std::move(exact);
		}
	}
	return best;
}

} // namespace

static void AddKnapsackFunction(ExtensionLoader &loader, const string &name,
                                Selection (*fn)(const vector<double> &, const vector<double> &,
                                                double),
                                bool exact, const string &description, const string &example, const string &alias_of) {
	auto return_type = LogicalType::STRUCT({{"selected", LogicalType::LIST(LogicalType::BOOLEAN)},
	                                        {"total_value", LogicalType::DOUBLE},
	                                        {"total_weight", LogicalType::DOUBLE}});

	ScalarFunction function(
	    duckdb_compat::FunctionName(name),
	    {LogicalType::LIST(LogicalType::DOUBLE), LogicalType::LIST(LogicalType::DOUBLE),
	     LogicalType::DOUBLE},
	    return_type, [fn, exact, name](DataChunk &args, ExpressionState &, Vector &result) {
#ifdef ANOFOX_TELEMETRY_ENABLED
		    PostHogTelemetry::Instance().RecordFunctionCall(name);
#endif
		    const idx_t count = args.size();
		    result.SetVectorType(VectorType::FLAT_VECTOR);

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
				    const auto plan = PlanExact(weights, capacity);
				    if (!plan.fits) {
					    throw InvalidInputException(
					        "exact knapsack needs a %.0f-cell table for %llu items at "
					        "capacity %f (weights scaled by %.0f to make them integral), "
					        "over the %llu-cell limit. Use opt_knapsack_greedy_ratio for "
					        "an approximate answer, or round the weights to coarser units.",
					        plan.cells, static_cast<uint64_t>(values.size()), capacity, plan.scale,
					        static_cast<uint64_t>(MAX_DP_CELLS));
				    }
				    sel = ExactDP(values, weights, plan);
			    } else {
				    sel = fn(values, weights, capacity);
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
                                              double),
                              bool exact, const string &description, const string &example) {
	const string canonical = "anofox_optimize_" + short_name;
	AddKnapsackFunction(loader, canonical, fn, exact, description, "anofox_optimize_" + example, "");
	AddKnapsackFunction(loader, "opt_" + short_name, fn, exact, description, "opt_" + example,
	                    canonical);
}

void RegisterKnapsackFunctions(ExtensionLoader &loader) {
	AddKnapsackFamily(
	    loader, "knapsack_greedy_ratio", GreedyRatio, false,
	    "0/1 knapsack by descending value-per-weight: takes each item that still fits. "
	    "Optimal for the FRACTIONAL problem and usually good for 0/1, but can be "
	    "arbitrarily bad — one heavy high-ratio item can crowd out a better "
	    "combination. Returns a boolean per input item plus the selected total value "
	    "and weight.",
	    "knapsack_greedy_ratio([10.0, 6.0], [5.0, 4.0], 6.0)");
	AddKnapsackFamily(
	    loader, "knapsack_greedy_value", GreedyValue, false,
	    "0/1 knapsack by descending VALUE, ignoring weight: takes each item that still "
	    "fits. The weakest member of the family — it loses to by-ratio whenever value "
	    "and weight are correlated — and included so a search has a poor-but-valid "
	    "option to move away from. Same signature as the other knapsack functions.",
	    "knapsack_greedy_value([10.0, 6.0], [5.0, 4.0], 6.0)");
	AddKnapsackFamily(
	    loader, "knapsack_exact", GreedyRatio, true,
	    "0/1 knapsack solved by dynamic programming over integer-scaled weights. "
	    "Returns the proven optimum when every weight and the capacity are whole "
	    "multiples of 0.0001, unlike the greedy members. Finer weights are rounded UP "
	    "to the next 0.0001 (and the capacity down), so the answer is always within "
	    "capacity but optimal only for the rounded-up weights. RAISES rather than "
	    "degrading when the table is too large — items * (capacity + 1) over 50 million, "
	    "counting weights in the smallest unit (1, 0.1, ... 0.0001) that makes them "
	    "whole — since a caller who asked for the exact answer and silently received "
	    "a heuristic one could not tell its result is no longer a bound. Same signature "
	    "as the other knapsack functions.",
	    "knapsack_exact([10.0, 6.0], [5.0, 4.0], 6.0)");
	AddKnapsackFamily(
	    loader, "knapsack_best_of", BestOfKnapsack, false,
	    "Runs both greedy knapsack members and, when the instance is small enough for "
	    "knapsack_exact, the exact dynamic program too, and returns whichever captured "
	    "the most value. Small enough means items * (capacity + 1) is at most 50 "
	    "million, with weights and capacity counted in the smallest unit (1, 0.1, ... "
	    "0.0001) that makes them whole numbers — e.g. 500 items at capacity 99,999. "
	    "There the result is the proven optimum when every weight and the capacity are "
	    "whole multiples of 0.0001; finer weights are rounded up, and the result is then "
	    "always within capacity but optimal only for the rounded-up weights. Above the "
	    "limit best_of does NOT raise but silently returns the best greedy answer, "
	    "which is NOT exact and carries no guarantee — call knapsack_exact if you need "
	    "to know which case you are in. The DP costs about 5 ns per table cell — roughly "
	    "a quarter second per row at the limit, against microseconds for the greedy "
	    "members — and is skipped only when greedy provably is the optimum: every "
	    "positive-value item fits, or the best-ratio prefix fills the capacity exactly. "
	    "Never loses to any single member. "
	    "Same signature as the other knapsack functions.",
	    "knapsack_best_of([10.0, 6.0], [5.0, 4.0], 6.0)");
}

} // namespace duckdb
