#include "knapsack.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

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
//! that its result is no longer a bound.
constexpr idx_t MAX_DP_CELLS = 50u * 1000u * 1000u;

//! Exact 0/1 knapsack by dynamic programming over integer-scaled weights.
Selection ExactDP(const vector<double> &values, const vector<double> &weights, double capacity,
                  double scale) {
	const idx_t n = values.size();
	const idx_t cap = static_cast<idx_t>(std::llround(capacity * scale));
	vector<idx_t> w(n);
	for (idx_t i = 0; i < n; i++) {
		w[i] = static_cast<idx_t>(std::llround(weights[i] * scale));
	}

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

Selection BestOfKnapsack(const vector<double> &values, const vector<double> &weights,
                         double capacity) {
	auto best = GreedyRatio(values, weights, capacity);
	auto by_value = GreedyValue(values, weights, capacity);
	if (by_value.total_value > best.total_value) {
		best = by_value;
	}
	return best;
}

} // namespace

static void AddKnapsackFunction(ExtensionLoader &loader, const string &name,
                                Selection (*fn)(const vector<double> &, const vector<double> &,
                                                double),
                                bool exact, const string &description, const string &example) {
	auto return_type = LogicalType::STRUCT({{"selected", LogicalType::LIST(LogicalType::BOOLEAN)},
	                                        {"total_value", LogicalType::DOUBLE},
	                                        {"total_weight", LogicalType::DOUBLE}});

	ScalarFunction function(
	    name,
	    {LogicalType::LIST(LogicalType::DOUBLE), LogicalType::LIST(LogicalType::DOUBLE),
	     LogicalType::DOUBLE},
	    return_type, [fn, exact, name](DataChunk &args, ExpressionState &, Vector &result) {
#ifdef ANOFOX_TELEMETRY_ENABLED
		    PostHogTelemetry::Instance().RecordFunctionCall(name);
#endif
		    const idx_t count = args.size();
		    result.SetVectorType(VectorType::FLAT_VECTOR);

		    UnifiedVectorFormat v_data, w_data, c_data;
		    args.data[0].ToUnifiedFormat(count, v_data);
		    args.data[1].ToUnifiedFormat(count, w_data);
		    args.data[2].ToUnifiedFormat(count, c_data);
		    auto v_lists = UnifiedVectorFormat::GetData<list_entry_t>(v_data);
		    auto w_lists = UnifiedVectorFormat::GetData<list_entry_t>(w_data);
		    auto caps = UnifiedVectorFormat::GetData<double>(c_data);

		    auto &v_child = ListVector::GetEntry(args.data[0]);
		    auto &w_child = ListVector::GetEntry(args.data[1]);
		    UnifiedVectorFormat vc, wc;
		    v_child.ToUnifiedFormat(ListVector::GetListSize(args.data[0]), vc);
		    w_child.ToUnifiedFormat(ListVector::GetListSize(args.data[1]), wc);
		    auto v_vals = UnifiedVectorFormat::GetData<double>(vc);
		    auto w_vals = UnifiedVectorFormat::GetData<double>(wc);

		    auto &entries = StructVector::GetEntries(result);
		    auto &sel_vec = *entries[0];
		    auto sel_out = FlatVector::GetData<list_entry_t>(sel_vec);
		    auto value_out = FlatVector::GetData<double>(*entries[1]);
		    auto weight_out = FlatVector::GetData<double>(*entries[2]);
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
				    // Scale to integers for the DP table. Refuse rather than
				    // silently degrade: a caller who asked for the exact answer
				    // and got a heuristic one cannot tell it is no longer a bound.
				    double scale = 1.0;
				    for (int p = 0; p < 4; p++) {
					    bool integral = true;
					    for (auto wgt : weights) {
						    if (std::fabs(wgt * scale - std::llround(wgt * scale)) > 1e-9) {
							    integral = false;
							    break;
						    }
					    }
					    if (integral && std::fabs(capacity * scale -
					                              std::llround(capacity * scale)) <= 1e-9) {
						    break;
					    }
					    scale *= 10.0;
				    }
				    const double cells =
				        static_cast<double>(values.size()) * (capacity * scale + 1);
				    if (!(cells <= static_cast<double>(MAX_DP_CELLS))) {
					    throw InvalidInputException(
					        "exact knapsack needs a %.0f-cell table for %llu items at "
					        "capacity %f (weights scaled by %.0f to make them integral), "
					        "over the %llu-cell limit. Use opt_knapsack_greedy_ratio for "
					        "an approximate answer, or round the weights to coarser units.",
					        cells, static_cast<uint64_t>(values.size()), capacity, scale,
					        static_cast<uint64_t>(MAX_DP_CELLS));
				    }
				    sel = ExactDP(values, weights, capacity, scale);
			    } else {
				    sel = fn(values, weights, capacity);
			    }

			    value_out[row] = sel.total_value;
			    weight_out[row] = sel.total_weight;
			    sel_out[row].offset = sel_offset;
			    sel_out[row].length = sel.taken.size();
			    ListVector::Reserve(sel_vec, sel_offset + sel.taken.size());
			    auto sel_child = FlatVector::GetData<bool>(ListVector::GetEntry(sel_vec));
			    for (idx_t i = 0; i < sel.taken.size(); i++) {
				    sel_child[sel_offset + i] = sel.taken[i];
			    }
			    sel_offset += sel.taken.size();
		    }
		    FlatVector::Validity(ListVector::GetEntry(sel_vec)).SetAllValid(sel_offset);
		    ListVector::SetListSize(sel_vec, sel_offset);
	    });

	FunctionDescription desc;
	desc.description = description;
	desc.examples.push_back(example);
	CreateScalarFunctionInfo info(function);
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

static void AddKnapsackFamily(ExtensionLoader &loader, const string &short_name,
                              Selection (*fn)(const vector<double> &, const vector<double> &,
                                              double),
                              bool exact, const string &description, const string &example) {
	AddKnapsackFunction(loader, "anofox_optimize_" + short_name, fn, exact, description,
	                    "anofox_optimize_" + example);
	AddKnapsackFunction(loader, "opt_" + short_name, fn, exact, description, "opt_" + example);
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
	    "0/1 knapsack solved EXACTLY by dynamic programming over integer-scaled "
	    "weights. Returns the true optimum, unlike the greedy members. RAISES rather "
	    "than degrading when the instance needs too large a table (many items, large "
	    "capacity, or weights needing fine scaling) — a caller who asked for the exact "
	    "answer and silently received a heuristic one could not tell its result is no "
	    "longer a bound. Same signature as the other knapsack functions.",
	    "knapsack_exact([10.0, 6.0], [5.0, 4.0], 6.0)");
	AddKnapsackFamily(
	    loader, "knapsack_best_of", BestOfKnapsack, false,
	    "Runs the greedy knapsack members and returns whichever captured more value. "
	    "Cheap and never loses to any single greedy member, but it is NOT exact — use "
	    "knapsack_exact when the instance is small enough. Same signature as the other "
	    "knapsack functions.",
	    "knapsack_best_of([10.0, 6.0], [5.0, 4.0], 6.0)");
}

} // namespace duckdb
