#include "selection.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

#include "register_alias.hpp"

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

//! An assortment objective: what a listed set is worth. Two genuinely
//! different real decisions share this shape, and they are close to
//! opposites, so which one a caller wants is never inferable from the
//! inputs — the caller picks by choosing a function family.
using AssortmentObjective = double (*)(const vector<double> &, const vector<double> &,
                                       const vector<double> &, idx_t, const vector<bool> &);

//! CANNIBALISATION objective: each listed product earns
//! margin * (base_demand - demand lost to the OTHER LISTED products).
//! The decision is "these products steal from each other, so do not
//! shelf near-duplicates". The quadratic term is what a separable
//! knapsack cannot represent.
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

//! RECAPTURE objective: every listed product keeps its own margin *
//! base_demand in full, and additionally earns the demand that DELISTED
//! products hand to it, captured at the LISTED product's own margin.
//! The decision is "we must drop products; which survivors soak up the
//! orphaned demand most profitably".
//!
//! This is not a variant of the cannibalisation objective, it is close
//! to its opposite: there, listed neighbours are a liability; here, a
//! listed product is worth MORE when the products it substitutes for are
//! delisted. A high-margin, low-demand product can be worth listing
//! purely as a recapture sink — which is precisely the choice a
//! margin*demand ranking cannot see.
double RecapturedMargin(const vector<double> &margin, const vector<double> &demand,
                        const vector<double> &sub, idx_t n, const vector<bool> &listed) {
	double total = 0;
	for (idx_t i = 0; i < n; i++) {
		if (listed[i]) {
			total += margin[i] * demand[i];
		}
	}
	for (idx_t j = 0; j < n; j++) {
		if (listed[j]) {
			continue;
		}
		for (idx_t i = 0; i < n; i++) {
			if (i != j && listed[i]) {
				total += demand[j] * sub[j * n + i] * margin[i];
			}
		}
	}
	return total;
}

Assortment Score(AssortmentObjective obj, const vector<double> &margin,
                 const vector<double> &demand, const vector<double> &sub, idx_t n,
                 vector<bool> listed) {
	Assortment a;
	a.captured_margin = obj(margin, demand, sub, n, listed);
	a.listed = std::move(listed);
	return a;
}

//! Top-K by standalone margin*demand, ignoring cannibalisation. The
//! obvious rule, and the one that overstates its own result — a search
//! needs it present to have something to beat.
Assortment TopMargin(AssortmentObjective obj, const vector<double> &margin,
                     const vector<double> &demand, const vector<double> &sub, idx_t n,
                     idx_t max_listed) {
	vector<idx_t> order(n);
	std::iota(order.begin(), order.end(), 0);
	std::stable_sort(order.begin(), order.end(),
	                 [&](idx_t a, idx_t b) { return margin[a] * demand[a] > margin[b] * demand[b]; });
	vector<bool> listed(n, false);
	for (idx_t k = 0; k < std::min(max_listed, n); k++) {
		listed[order[k]] = true;
	}
	return Score(obj, margin, demand, sub, n, std::move(listed));
}

//! Add whichever product raises TOTAL captured margin most, accounting
//! for what it takes from the products already listed. Stops when no
//! addition helps, even below the shelf limit — listing a pure
//! cannibaliser loses money.
Assortment GreedyMarginal(AssortmentObjective obj, const vector<double> &margin,
                          const vector<double> &demand, const vector<double> &sub, idx_t n,
                          idx_t max_listed) {
	vector<bool> listed(n, false);
	// The empty set is not worth zero under every objective (recapture
	// pays nothing when nothing is listed, but the baseline must still
	// come from the objective, not be assumed).
	double best_total = obj(margin, demand, sub, n, listed);
	for (idx_t k = 0; k < std::min(max_listed, n); k++) {
		idx_t best_item = n;
		double best_gain = 0;
		for (idx_t i = 0; i < n; i++) {
			if (listed[i]) {
				continue;
			}
			listed[i] = true;
			const double total = obj(margin, demand, sub, n, listed);
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
	return Score(obj, margin, demand, sub, n, std::move(listed));
}

//! Greedy, then swap a listed product for an unlisted one while it helps.
Assortment SelectionLocalSearch(AssortmentObjective obj, const vector<double> &margin,
                                const vector<double> &demand, const vector<double> &sub, idx_t n,
                                idx_t max_listed) {
	auto best = GreedyMarginal(obj, margin, demand, sub, n, max_listed);
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
				const double total = obj(margin, demand, sub, n, trial);
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

Assortment BestOfAssortment(AssortmentObjective obj, const vector<double> &margin,
                            const vector<double> &demand, const vector<double> &sub, idx_t n,
                            idx_t max_listed) {
	auto best = SelectionLocalSearch(obj, margin, demand, sub, n, max_listed);
	for (auto c : {GreedyMarginal(obj, margin, demand, sub, n, max_listed),
	               TopMargin(obj, margin, demand, sub, n, max_listed)}) {
		if (c.captured_margin > best.captured_margin) {
			best = c;
		}
	}
	return best;
}

//! Concrete entry points. Each family binds one objective; the
//! algorithms above are shared verbatim, so a fix to the search benefits
//! both models and neither can silently drift from the other.
#define ANOFOX_ASSORTMENT_BINDING(suffix, objective)                                               \
	Assortment TopMargin##suffix(const vector<double> &m, const vector<double> &d,                 \
	                             const vector<double> &s, idx_t n, idx_t k) {                      \
		return TopMargin(objective, m, d, s, n, k);                                                \
	}                                                                                              \
	Assortment GreedyMarginal##suffix(const vector<double> &m, const vector<double> &d,            \
	                                  const vector<double> &s, idx_t n, idx_t k) {                 \
		return GreedyMarginal(objective, m, d, s, n, k);                                           \
	}                                                                                              \
	Assortment LocalSearch##suffix(const vector<double> &m, const vector<double> &d,               \
	                               const vector<double> &s, idx_t n, idx_t k) {                    \
		return SelectionLocalSearch(objective, m, d, s, n, k);                                     \
	}                                                                                              \
	Assortment BestOf##suffix(const vector<double> &m, const vector<double> &d,                    \
	                          const vector<double> &s, idx_t n, idx_t k) {                         \
		return BestOfAssortment(objective, m, d, s, n, k);                                         \
	}

ANOFOX_ASSORTMENT_BINDING(Cannibalisation, CapturedMargin)
ANOFOX_ASSORTMENT_BINDING(Recapture, RecapturedMargin)

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

// ---------------------------------------------------------------- P7 --

struct Portfolio {
	vector<double> weights;
	double expected_return = 0;
	double volatility = 0;
	double sharpe = 0;
};

double PortfolioVariance(const vector<double> &cov, idx_t n, const vector<double> &w) {
	double v = 0;
	for (idx_t i = 0; i < n; i++) {
		if (w[i] == 0) {
			continue;
		}
		for (idx_t j = 0; j < n; j++) {
			v += w[i] * w[j] * cov[i * n + j];
		}
	}
	return v;
}

Portfolio ScorePortfolio(const vector<double> &ret, const vector<double> &cov, idx_t n,
                         vector<double> w) {
	Portfolio p;
	p.weights = std::move(w);
	for (idx_t i = 0; i < n; i++) {
		p.expected_return += p.weights[i] * ret[i];
	}
	p.volatility = std::sqrt(std::max(0.0, PortfolioVariance(cov, n, p.weights)));
	p.sharpe = p.volatility > 1e-12 ? p.expected_return / p.volatility : 0.0;
	return p;
}

//! Project onto {sum w = 1, 0 <= w <= cap} over a fixed support.
vector<double> ProjectWeights(vector<double> w, const vector<idx_t> &support, double cap) {
	for (int iter = 0; iter < 200; iter++) {
		double total = 0;
		for (auto i : support) {
			w[i] = std::min(cap, std::max(0.0, w[i]));
			total += w[i];
		}
		if (std::fabs(total - 1.0) < 1e-12) {
			break;
		}
		const double adjust = (1.0 - total) / static_cast<double>(support.size());
		for (auto i : support) {
			w[i] += adjust;
		}
	}
	return w;
}

//! Maximise Sharpe over a FIXED support by projected gradient ascent.
Portfolio OptimiseSupport(const vector<double> &ret, const vector<double> &cov, idx_t n,
                          const vector<idx_t> &support, double cap) {
	vector<double> w(n, 0.0);
	for (auto i : support) {
		w[i] = 1.0 / static_cast<double>(support.size());
	}
	w = ProjectWeights(std::move(w), support, cap);
	double step = 0.05;
	auto best = ScorePortfolio(ret, cov, n, w);
	for (int iter = 0; iter < 400; iter++) {
		const double var = std::max(1e-18, PortfolioVariance(cov, n, w));
		const double vol = std::sqrt(var);
		const double mu = best.expected_return;
		vector<double> grad(n, 0.0);
		for (auto i : support) {
			double cw = 0;
			for (idx_t j = 0; j < n; j++) {
				cw += cov[i * n + j] * w[j];
			}
			grad[i] = ret[i] / vol - mu * cw / (var * vol);
		}
		auto trial = w;
		for (auto i : support) {
			trial[i] += step * grad[i];
		}
		trial = ProjectWeights(std::move(trial), support, cap);
		auto scored = ScorePortfolio(ret, cov, n, trial);
		if (scored.sharpe > best.sharpe) {
			best = scored;
			w = std::move(trial);
		} else {
			step *= 0.7;
			if (step < 1e-9) {
				break;
			}
		}
	}
	return best;
}

vector<idx_t> TopKByReturn(const vector<double> &ret, idx_t n, idx_t k) {
	vector<idx_t> order(n);
	std::iota(order.begin(), order.end(), 0);
	std::stable_sort(order.begin(), order.end(), [&](idx_t a, idx_t b) { return ret[a] > ret[b]; });
	order.resize(std::min(k, n));
	return order;
}

//! Top-K by expected return, equally weighted. Ignores covariance
//! entirely, so it happily buys K assets that all move together.
Portfolio TopReturnEqualWeight(const vector<double> &ret, const vector<double> &cov, idx_t n,
                               idx_t k, double cap) {
	auto support = TopKByReturn(ret, n, k);
	if (support.empty()) {
		return Portfolio {vector<double>(n, 0.0), 0, 0, 0};
	}
	vector<double> w(n, 0.0);
	for (auto i : support) {
		w[i] = 1.0 / static_cast<double>(support.size());
	}
	return ScorePortfolio(ret, cov, n, ProjectWeights(std::move(w), support, cap));
}

//! Top-K by return, then optimize the WEIGHTS on that support.
Portfolio TopReturnOptimised(const vector<double> &ret, const vector<double> &cov, idx_t n, idx_t k,
                             double cap) {
	auto support = TopKByReturn(ret, n, k);
	if (support.empty()) {
		return Portfolio {vector<double>(n, 0.0), 0, 0, 0};
	}
	return OptimiseSupport(ret, cov, n, support, cap);
}

//! Grow the support greedily by whichever asset most improves Sharpe
//! after re-optimizing weights — so it can pick a lower-return asset
//! that diversifies. This is the member that uses the covariance.
Portfolio GreedySharpe(const vector<double> &ret, const vector<double> &cov, idx_t n, idx_t k,
                       double cap) {
	const idx_t limit = std::min(k, n);
	if (limit == 0) {
		return Portfolio {vector<double>(n, 0.0), 0, 0, 0};
	}
	// Weights sum to 1 and none may exceed `cap`, so no support smaller
	// than ceil(1/cap) can hold a feasible portfolio at all. Sharpe is
	// meaningless below that size, so the set is grown by expected return
	// until it is feasible and only then greedily on Sharpe.
	//
	// Testing feasibility on every INTERMEDIATE support instead skipped
	// every single-asset candidate on the first step and returned an
	// empty portfolio scoring 0 — caught by the family-spread check.
	const idx_t min_support =
	    static_cast<idx_t>(std::ceil(1.0 / cap - 1e-9));
	vector<idx_t> support;
	auto by_return = TopKByReturn(ret, n, n);
	for (auto i : by_return) {
		if (support.size() >= std::min(min_support, limit)) {
			break;
		}
		support.push_back(i);
	}
	if (support.size() < min_support) {
		// Cannot be made feasible within the holding limit; the caller-facing
		// guard rejects this before we get here, so this is defence only.
		return Portfolio {vector<double>(n, 0.0), 0, 0, 0};
	}
	Portfolio best = OptimiseSupport(ret, cov, n, support, cap);

	while (support.size() < limit) {
		idx_t best_add = n;
		Portfolio best_trial = best;
		for (idx_t i = 0; i < n; i++) {
			if (std::find(support.begin(), support.end(), i) != support.end()) {
				continue;
			}
			auto trial_support = support;
			trial_support.push_back(i);
			auto trial = OptimiseSupport(ret, cov, n, trial_support, cap);
			if (trial.sharpe > best_trial.sharpe + 1e-12) {
				best_trial = trial;
				best_add = i;
			}
		}
		if (best_add == n) {
			break;
		}
		support.push_back(best_add);
		best = best_trial;
	}

	// Then SWAP: replace a held asset with an unheld one while that
	// improves Sharpe. Growth alone is not enough — when the holding
	// limit equals the minimum feasible support (k=2 at a 0.6 cap, say)
	// there is no room to grow, so a purely additive greedy can only ever
	// return its return-ranked seed and ties the naive member exactly.
	// Substitution is what lets it drop a high-return asset for a hedge.
	bool improved = true;
	while (improved) {
		improved = false;
		for (idx_t si = 0; si < support.size() && !improved; si++) {
			for (idx_t j = 0; j < n; j++) {
				if (std::find(support.begin(), support.end(), j) != support.end()) {
					continue;
				}
				auto trial_support = support;
				trial_support[si] = j;
				auto trial = OptimiseSupport(ret, cov, n, trial_support, cap);
				if (trial.sharpe > best.sharpe + 1e-12) {
					support = std::move(trial_support);
					best = trial;
					improved = true;
					break;
				}
			}
		}
	}
	return best;
}

Portfolio BestOfPortfolio(const vector<double> &ret, const vector<double> &cov, idx_t n, idx_t k,
                          double cap) {
	auto best = GreedySharpe(ret, cov, n, k, cap);
	for (auto c : {TopReturnOptimised(ret, cov, n, k, cap),
	               TopReturnEqualWeight(ret, cov, n, k, cap)}) {
		if (c.sharpe > best.sharpe) {
			best = c;
		}
	}
	return best;
}

} // namespace

static void AddPortfolioFunction(ExtensionLoader &loader, const string &name,
                                 Portfolio (*fn)(const vector<double> &, const vector<double> &,
                                                 idx_t, idx_t, double),
                                 const string &description, const string &example, const string &alias_of) {
	auto return_type = LogicalType::STRUCT({{"weights", LogicalType::LIST(LogicalType::DOUBLE)},
	                                        {"expected_return", LogicalType::DOUBLE},
	                                        {"volatility", LogicalType::DOUBLE},
	                                        {"sharpe", LogicalType::DOUBLE}});
	ScalarFunction function(
	    name,
	    {LogicalType::LIST(LogicalType::DOUBLE), LogicalType::LIST(LogicalType::DOUBLE),
	     LogicalType::BIGINT, LogicalType::DOUBLE},
	    return_type, [fn, name](DataChunk &args, ExpressionState &, Vector &result) {
#ifdef ANOFOX_TELEMETRY_ENABLED
		    PostHogTelemetry::Instance().RecordFunctionCall(name);
#endif
		    const idx_t count = args.size();
		    result.SetVectorType(VectorType::FLAT_VECTOR);
		    UnifiedVectorFormat k_data, cap_data;
		    args.data[2].ToUnifiedFormat(count, k_data);
		    args.data[3].ToUnifiedFormat(count, cap_data);
		    auto ks = UnifiedVectorFormat::GetData<int64_t>(k_data);
		    auto caps = UnifiedVectorFormat::GetData<double>(cap_data);

		    auto &entries = StructVector::GetEntries(result);
		    auto &w_vec = *entries[0];
		    auto w_out = FlatVector::GetData<list_entry_t>(w_vec);
		    auto ret_out = FlatVector::GetData<double>(*entries[1]);
		    auto vol_out = FlatVector::GetData<double>(*entries[2]);
		    auto sharpe_out = FlatVector::GetData<double>(*entries[3]);
		    idx_t offset = 0;

		    for (idx_t row = 0; row < count; row++) {
			    const auto ki = k_data.sel->get_index(row);
			    const auto ci = cap_data.sel->get_index(row);
			    if (!k_data.validity.RowIsValid(ki) || !cap_data.validity.RowIsValid(ci)) {
				    FlatVector::SetNull(result, row, true);
				    w_out[row].offset = offset;
				    w_out[row].length = 0;
				    ret_out[row] = vol_out[row] = sharpe_out[row] = 0;
				    continue;
			    }
			    if (ks[ki] < 0) {
				    throw InvalidInputException("max_holdings must be >= 0, got %lld",
				                                static_cast<long long>(ks[ki]));
			    }
			    const double cap = caps[ci];
			    if (!std::isfinite(cap) || cap <= 0 || cap > 1.0) {
				    throw InvalidInputException(
				        "max_position_size must be in (0,1], got %f — weights sum to 1, so a "
				        "cap outside that range is either unsatisfiable or no constraint",
				        cap);
			    }
			    auto ret = ReadDoubles(args.data[0], count, row, "expected_returns", 0);
			    const idx_t n = ret.size();
			    auto cov = ReadDoubles(args.data[1], count, row, "covariance matrix", n * n);
			    const idx_t k = static_cast<idx_t>(ks[ki]);
			    if (n > 0 && static_cast<double>(std::min(k, n)) * cap < 1.0 - 1e-12) {
				    throw InvalidInputException(
				        "max_holdings %llu at max_position_size %f can hold at most %f of the "
				        "portfolio — weights must sum to 1, so no feasible portfolio exists",
				        static_cast<uint64_t>(k), cap, static_cast<double>(std::min(k, n)) * cap);
			    }

			    const auto p = n == 0 ? Portfolio {} : fn(ret, cov, n, k, cap);
			    ret_out[row] = p.expected_return;
			    vol_out[row] = p.volatility;
			    sharpe_out[row] = p.sharpe;
			    w_out[row].offset = offset;
			    w_out[row].length = p.weights.size();
			    ListVector::Reserve(w_vec, offset + p.weights.size());
			    auto child = FlatVector::GetData<double>(ListVector::GetEntry(w_vec));
			    for (idx_t i = 0; i < p.weights.size(); i++) {
				    child[offset + i] = p.weights[i];
			    }
			    offset += p.weights.size();
		    }
		    FlatVector::Validity(ListVector::GetEntry(w_vec)).SetAllValid(offset);
		    ListVector::SetListSize(w_vec, offset);
	    });

	RegisterScalarOrAlias(loader, std::move(function), description, example, alias_of);
}

static void AddPortfolioFamily(ExtensionLoader &loader, const string &short_name,
                               Portfolio (*fn)(const vector<double> &, const vector<double> &,
                                               idx_t, idx_t, double),
                               const string &description, const string &example) {
	const string canonical = "anofox_optimize_" + short_name;
	AddPortfolioFunction(loader, canonical, fn, description, "anofox_optimize_" + example, "");
	AddPortfolioFunction(loader, "opt_" + short_name, fn, description, "opt_" + example, canonical);
}

static void AddAssortmentFunction(ExtensionLoader &loader, const string &name,
                                  Assortment (*fn)(const vector<double> &, const vector<double> &,
                                                   const vector<double> &, idx_t, idx_t),
                                  const string &description, const string &example, const string &alias_of) {
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

	RegisterScalarOrAlias(loader, std::move(function), description, example, alias_of);
}

static void AddAssortmentFamily(ExtensionLoader &loader, const string &short_name,
                                Assortment (*fn)(const vector<double> &, const vector<double> &,
                                                 const vector<double> &, idx_t, idx_t),
                                const string &description, const string &example) {
	const string canonical = "anofox_optimize_" + short_name;
	AddAssortmentFunction(loader, canonical, fn, description, "anofox_optimize_" + example, "");
	AddAssortmentFunction(loader, "opt_" + short_name, fn, description, "opt_" + example, canonical);
}

void RegisterSelectionFunctions(ExtensionLoader &loader) {
	// The two assortment families take IDENTICAL arguments and return an
	// identical struct, but score a listed set under opposite economics.
	// Nothing in the data says which one a caller means, so each
	// description states its model in full rather than deferring to a
	// shared sentence — picking the wrong family is silent and costly.
	const string args =
	    " Takes margins and base_demands (one per product) plus the substitution matrix "
	    "flattened ROW-MAJOR — substitution[j*n+i] is the fraction of product j's demand "
	    "that moves to product i — and a shelf limit. Returns a boolean per product and "
	    "the resulting captured margin.";
	const string shape =
	    " MODEL: CANNIBALISATION — each listed product earns margin * (base_demand minus "
	    "the demand the OTHER LISTED products take from it). Listing near-duplicates "
	    "destroys value here. If instead you are DELISTING and want the demand of dropped "
	    "products to flow to the survivors, use the anofox_optimize_assortment_recapture_* "
	    "family: it answers a different question and the two disagree." +
	    args;
	const string rshape =
	    " MODEL: RECAPTURE — each listed product keeps margin * base_demand IN FULL, and "
	    "additionally earns the demand handed to it by DELISTED products, valued at the "
	    "LISTED product's own margin. A high-margin, low-demand product can be worth "
	    "listing purely as a recapture sink, which a margin*demand ranking never selects. "
	    "If instead listed products erode each other, use the "
	    "anofox_optimize_assortment_* family: it answers a different question and the two "
	    "disagree." +
	    args;
	const string ex = "([5.0,4.0],[100.0,90.0],[0.0,0.6,0.6,0.0], 2)";

	AddAssortmentFamily(loader, "assortment_top_margin", TopMarginCannibalisation,
	                    "Lists the top products by standalone margin*demand, IGNORING "
	                    "cannibalisation. The obvious rule, and the one that overstates its "
	                    "own result whenever listed products substitute for each other — "
	                    "included so a search has something to beat." + shape,
	                    "assortment_top_margin" + ex);
	AddAssortmentFamily(loader, "assortment_greedy_marginal", GreedyMarginalCannibalisation,
	                    "Lists products one at a time, each time adding whichever raises TOTAL "
	                    "captured margin most given what it takes from the products already "
	                    "listed. Stops early when no addition helps, even below the shelf "
	                    "limit: listing a pure cannibaliser loses money." + shape,
	                    "assortment_greedy_marginal" + ex);
	AddAssortmentFamily(loader, "assortment_local_search", LocalSearchCannibalisation,
	                    "Greedy marginal, then swaps a listed product for an unlisted one "
	                    "while that raises captured margin. Escapes the greedy ordering, at "
	                    "O(n^2) evaluations per improving pass." + shape,
	                    "assortment_local_search" + ex);
	AddAssortmentFamily(loader, "assortment_best_of", BestOfCannibalisation,
	                    "Runs every assortment algorithm in this family and returns whichever "
	                    "captured the most margin." + shape,
	                    "assortment_best_of" + ex);

	AddAssortmentFamily(loader, "assortment_recapture_top_margin", TopMarginRecapture,
	                    "Lists the top products by standalone margin*base_demand, IGNORING "
	                    "where delisted demand would go. The obvious rule, and a poor one "
	                    "here: it never lists a small-demand product that would soak up a "
	                    "lot of orphaned demand at a high margin. Included so a search has "
	                    "something to beat." + rshape,
	                    "assortment_recapture_top_margin" + ex);
	AddAssortmentFamily(loader, "assortment_recapture_greedy_marginal", GreedyMarginalRecapture,
	                    "Lists products one at a time, each time adding whichever raises TOTAL "
	                    "recaptured margin most — which accounts for the demand that product "
	                    "stops donating to others once it is listed itself. Stops early when "
	                    "no addition helps, even below the shelf limit." + rshape,
	                    "assortment_recapture_greedy_marginal" + ex);
	AddAssortmentFamily(loader, "assortment_recapture_local_search", LocalSearchRecapture,
	                    "Recapture greedy, then swaps a listed product for an unlisted one "
	                    "while that raises recaptured margin. Escapes the greedy ordering, at "
	                    "O(n^2) evaluations per improving pass." + rshape,
	                    "assortment_recapture_local_search" + ex);
	AddAssortmentFamily(loader, "assortment_recapture_best_of", BestOfRecapture,
	                    "Runs every recapture algorithm in this family and returns whichever "
	                    "recaptured the most margin." + rshape,
	                    "assortment_recapture_best_of" + ex);

	const string pshape =
	    " Takes expected_returns (one per asset) and the covariance matrix flattened "
	    "ROW-MAJOR, a maximum number of holdings, and a per-asset weight cap. Weights "
	    "sum to 1 over the chosen assets. Returns the weight vector, expected return, "
	    "volatility and Sharpe ratio (return/volatility).";
	const string pex = "([0.12,0.10,0.10],[0.04,0.036,-0.01,0.036,0.04,-0.01,-0.01,-0.01,0.04], 2, 0.6)";

	AddPortfolioFamily(loader, "portfolio_top_return", TopReturnEqualWeight,
	                   "Picks the highest-expected-return assets up to the holding limit and "
	                   "weights them EQUALLY, ignoring covariance entirely — so it will "
	                   "happily buy assets that all move together. The naive rule, included "
	                   "so a search has something to beat." + pshape,
	                   "portfolio_top_return" + pex);
	AddPortfolioFamily(loader, "portfolio_top_return_optimized", TopReturnOptimised,
	                   "Picks the highest-expected-return assets up to the holding limit, then "
	                   "OPTIMISES THE WEIGHTS on that fixed set by projected gradient ascent "
	                   "on Sharpe. Better than equal weighting, but still cannot choose a "
	                   "lower-return asset that would diversify." + pshape,
	                   "portfolio_top_return_optimized" + pex);
	AddPortfolioFamily(loader, "portfolio_greedy_sharpe", GreedySharpe,
	                   "Grows the holding set one asset at a time, each time adding whichever "
	                   "most improves Sharpe AFTER re-optimizing the weights — so it can pick "
	                   "a lower-return asset because it diversifies — then SWAPS held assets "
	                   "for unheld ones while that improves Sharpe, which is what lets it "
	                   "choose a different set when the holding limit leaves no room to grow. "
	                   "The member that actually uses the covariance to choose WHICH assets, "
	                   "not just how much of them." + pshape,
	                   "portfolio_greedy_sharpe" + pex);
	AddPortfolioFamily(loader, "portfolio_best_of", BestOfPortfolio,
	                   "Runs every portfolio algorithm in this family and returns whichever "
	                   "achieved the highest Sharpe ratio." + pshape,
	                   "portfolio_best_of" + pex);
}

} // namespace duckdb
