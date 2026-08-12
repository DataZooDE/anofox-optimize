#include "selection.hpp"

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

// ---------------------------------------------------------------- P5 --

struct Assortment {
	vector<bool> listed;
	double captured_margin = 0;
};

//! Captured margin of a listed set: each listed product earns
//! margin * (base_demand - demand lost to the other listed products).
//! The quadratic term is what a separable knapsack cannot represent.
double CapturedMargin(const vector<double> &margin, const vector<double> &demand,
                      const vector<double> &sub, idx_t n, const vector<bool> &listed) {
	double total = 0;
	for (idx_t i = 0; i < n; i++) {
		if (!listed[i]) {
			continue;
		}
		double d = demand[i];
		for (idx_t j = 0; j < n; j++) {
			if (j != i && listed[j]) {
				d -= sub[j * n + i] * demand[j];
			}
		}
		total += margin[i] * std::max(0.0, d);
	}
	return total;
}

Assortment Score(const vector<double> &margin, const vector<double> &demand,
                 const vector<double> &sub, idx_t n, vector<bool> listed) {
	Assortment a;
	a.captured_margin = CapturedMargin(margin, demand, sub, n, listed);
	a.listed = std::move(listed);
	return a;
}

//! Top-K by standalone margin*demand, ignoring cannibalisation. The
//! obvious rule, and the one that overstates its own result — a search
//! needs it present to have something to beat.
Assortment TopMargin(const vector<double> &margin, const vector<double> &demand,
                     const vector<double> &sub, idx_t n, idx_t max_listed) {
	vector<idx_t> order(n);
	std::iota(order.begin(), order.end(), 0);
	std::stable_sort(order.begin(), order.end(),
	                 [&](idx_t a, idx_t b) { return margin[a] * demand[a] > margin[b] * demand[b]; });
	vector<bool> listed(n, false);
	for (idx_t k = 0; k < std::min(max_listed, n); k++) {
		listed[order[k]] = true;
	}
	return Score(margin, demand, sub, n, std::move(listed));
}

//! Add whichever product raises TOTAL captured margin most, accounting
//! for what it takes from the products already listed. Stops when no
//! addition helps, even below the shelf limit — listing a pure
//! cannibaliser loses money.
Assortment GreedyMarginal(const vector<double> &margin, const vector<double> &demand,
                          const vector<double> &sub, idx_t n, idx_t max_listed) {
	vector<bool> listed(n, false);
	double best_total = 0;
	for (idx_t k = 0; k < std::min(max_listed, n); k++) {
		idx_t best_item = n;
		double best_gain = 0;
		for (idx_t i = 0; i < n; i++) {
			if (listed[i]) {
				continue;
			}
			listed[i] = true;
			const double total = CapturedMargin(margin, demand, sub, n, listed);
			listed[i] = false;
			if (total - best_total > best_gain + 1e-12) {
				best_gain = total - best_total;
				best_item = i;
			}
		}
		if (best_item == n) {
			break;
		}
		listed[best_item] = true;
		best_total += best_gain;
	}
	return Score(margin, demand, sub, n, std::move(listed));
}

//! Greedy, then swap a listed product for an unlisted one while it helps.
Assortment SelectionLocalSearch(const vector<double> &margin, const vector<double> &demand,
                                const vector<double> &sub, idx_t n, idx_t max_listed) {
	auto best = GreedyMarginal(margin, demand, sub, n, max_listed);
	bool improved = true;
	while (improved) {
		improved = false;
		for (idx_t i = 0; i < n && !improved; i++) {
			if (!best.listed[i]) {
				continue;
			}
			for (idx_t j = 0; j < n; j++) {
				if (best.listed[j]) {
					continue;
				}
				auto trial = best.listed;
				trial[i] = false;
				trial[j] = true;
				const double total = CapturedMargin(margin, demand, sub, n, trial);
				if (total > best.captured_margin + 1e-12) {
					best.listed = std::move(trial);
					best.captured_margin = total;
					improved = true;
					break;
				}
			}
		}
	}
	return best;
}

Assortment BestOfAssortment(const vector<double> &margin, const vector<double> &demand,
                            const vector<double> &sub, idx_t n, idx_t max_listed) {
	auto best = SelectionLocalSearch(margin, demand, sub, n, max_listed);
	for (auto c : {GreedyMarginal(margin, demand, sub, n, max_listed),
	               TopMargin(margin, demand, sub, n, max_listed)}) {
		if (c.captured_margin > best.captured_margin) {
			best = c;
		}
	}
	return best;
}

vector<double> ReadDoubles(Vector &v, idx_t count, idx_t row, const char *what, idx_t expect) {
	UnifiedVectorFormat data;
	v.ToUnifiedFormat(count, data);
	auto lists = UnifiedVectorFormat::GetData<list_entry_t>(data);
	const auto li = data.sel->get_index(row);
	if (!data.validity.RowIsValid(li)) {
		throw InvalidInputException("%s must not be NULL", what);
	}
	auto &child = ListVector::GetEntry(v);
	UnifiedVectorFormat cd;
	child.ToUnifiedFormat(ListVector::GetListSize(v), cd);
	auto values = UnifiedVectorFormat::GetData<double>(cd);
	const auto entry = lists[li];
	if (expect && entry.length != expect) {
		throw InvalidInputException("%s has %llu entries but %llu were required", what,
		                            static_cast<uint64_t>(entry.length),
		                            static_cast<uint64_t>(expect));
	}
	vector<double> out;
	out.reserve(entry.length);
	for (idx_t i = 0; i < entry.length; i++) {
		const auto ci = cd.sel->get_index(entry.offset + i);
		if (!cd.validity.RowIsValid(ci)) {
			throw InvalidInputException("%s entry %llu is NULL — a NULL cannot be compared, so "
			                            "that item would be silently unselectable",
			                            what, static_cast<uint64_t>(i));
		}
		const double x = values[ci];
		if (!std::isfinite(x)) {
			throw InvalidInputException("%s entry %llu is %f — values must be finite", what,
			                            static_cast<uint64_t>(i), x);
		}
		out.push_back(x);
	}
	return out;
}

} // namespace

static void AddAssortmentFunction(ExtensionLoader &loader, const string &name,
                                  Assortment (*fn)(const vector<double> &, const vector<double> &,
                                                   const vector<double> &, idx_t, idx_t),
                                  const string &description, const string &example) {
	auto return_type = LogicalType::STRUCT({{"listed", LogicalType::LIST(LogicalType::BOOLEAN)},
	                                        {"captured_margin", LogicalType::DOUBLE}});
	ScalarFunction function(
	    name,
	    {LogicalType::LIST(LogicalType::DOUBLE), LogicalType::LIST(LogicalType::DOUBLE),
	     LogicalType::LIST(LogicalType::DOUBLE), LogicalType::BIGINT},
	    return_type, [fn, name](DataChunk &args, ExpressionState &, Vector &result) {
#ifdef ANOFOX_TELEMETRY_ENABLED
		    PostHogTelemetry::Instance().RecordFunctionCall(name);
#endif
		    const idx_t count = args.size();
		    result.SetVectorType(VectorType::FLAT_VECTOR);
		    UnifiedVectorFormat k_data;
		    args.data[3].ToUnifiedFormat(count, k_data);
		    auto ks = UnifiedVectorFormat::GetData<int64_t>(k_data);

		    auto &entries = StructVector::GetEntries(result);
		    auto &listed_vec = *entries[0];
		    auto listed_out = FlatVector::GetData<list_entry_t>(listed_vec);
		    auto margin_out = FlatVector::GetData<double>(*entries[1]);
		    idx_t offset = 0;

		    for (idx_t row = 0; row < count; row++) {
			    const auto ki = k_data.sel->get_index(row);
			    if (!k_data.validity.RowIsValid(ki)) {
				    FlatVector::SetNull(result, row, true);
				    listed_out[row].offset = offset;
				    listed_out[row].length = 0;
				    margin_out[row] = 0;
				    continue;
			    }
			    if (ks[ki] < 0) {
				    throw InvalidInputException("max_listed must be >= 0, got %lld",
				                                static_cast<long long>(ks[ki]));
			    }
			    auto margin = ReadDoubles(args.data[0], count, row, "margins", 0);
			    const idx_t n = margin.size();
			    auto demand = ReadDoubles(args.data[1], count, row, "base_demands", n);
			    auto sub = ReadDoubles(args.data[2], count, row, "substitution matrix", n * n);
			    for (auto s : sub) {
				    if (s < 0 || s > 1) {
					    throw InvalidInputException(
					        "substitution entries must be in [0,1], got %f — an entry outside "
					        "that range would create or destroy demand rather than move it",
					        s);
				    }
			    }

			    const auto a = n == 0 ? Assortment {}
			                          : fn(margin, demand, sub, n, static_cast<idx_t>(ks[ki]));
			    margin_out[row] = a.captured_margin;
			    listed_out[row].offset = offset;
			    listed_out[row].length = a.listed.size();
			    ListVector::Reserve(listed_vec, offset + a.listed.size());
			    auto child = FlatVector::GetData<bool>(ListVector::GetEntry(listed_vec));
			    for (idx_t i = 0; i < a.listed.size(); i++) {
				    child[offset + i] = a.listed[i];
			    }
			    offset += a.listed.size();
		    }
		    FlatVector::Validity(ListVector::GetEntry(listed_vec)).SetAllValid(offset);
		    ListVector::SetListSize(listed_vec, offset);
	    });

	FunctionDescription desc;
	desc.description = description;
	desc.examples.push_back(example);
	CreateScalarFunctionInfo info(function);
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

static void AddAssortmentFamily(ExtensionLoader &loader, const string &short_name,
                                Assortment (*fn)(const vector<double> &, const vector<double> &,
                                                 const vector<double> &, idx_t, idx_t),
                                const string &description, const string &example) {
	AddAssortmentFunction(loader, "anofox_optimize_" + short_name, fn, description,
	                      "anofox_optimize_" + example);
	AddAssortmentFunction(loader, "opt_" + short_name, fn, description, "opt_" + example);
}

void RegisterSelectionFunctions(ExtensionLoader &loader) {
	const string shape =
	    " Takes margins and base_demands (one per product) plus the substitution matrix "
	    "flattened ROW-MAJOR — substitution[j*n+i] is the fraction of product j's demand "
	    "that moves to product i when BOTH are listed — and a shelf limit. Returns a "
	    "boolean per product and the total captured margin, where each listed product "
	    "earns margin * (base_demand minus demand lost to the other listed products).";
	const string ex = "([5.0,4.0],[100.0,90.0],[0.0,0.6,0.6,0.0], 2)";

	AddAssortmentFamily(loader, "assortment_top_margin", TopMargin,
	                    "Lists the top products by standalone margin*demand, IGNORING "
	                    "cannibalisation. The obvious rule, and the one that overstates its "
	                    "own result whenever listed products substitute for each other — "
	                    "included so a search has something to beat." + shape,
	                    "assortment_top_margin" + ex);
	AddAssortmentFamily(loader, "assortment_greedy_marginal", GreedyMarginal,
	                    "Lists products one at a time, each time adding whichever raises TOTAL "
	                    "captured margin most given what it takes from the products already "
	                    "listed. Stops early when no addition helps, even below the shelf "
	                    "limit: listing a pure cannibaliser loses money." + shape,
	                    "assortment_greedy_marginal" + ex);
	AddAssortmentFamily(loader, "assortment_local_search", SelectionLocalSearch,
	                    "Greedy marginal, then swaps a listed product for an unlisted one "
	                    "while that raises captured margin. Escapes the greedy ordering, at "
	                    "O(n^2) evaluations per improving pass." + shape,
	                    "assortment_local_search" + ex);
	AddAssortmentFamily(loader, "assortment_best_of", BestOfAssortment,
	                    "Runs every assortment algorithm in this family and returns whichever "
	                    "captured the most margin." + shape,
	                    "assortment_best_of" + ex);
}

} // namespace duckdb
