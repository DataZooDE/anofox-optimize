#include "sequencing.hpp"

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

struct Sequence {
	vector<idx_t> order;
	double total_setup = 0;
};

double TotalSetup(const vector<double> &m, idx_t n, const vector<idx_t> &order) {
	double total = 0;
	for (idx_t k = 1; k < order.size(); k++) {
		total += m[order[k - 1] * n + order[k]];
	}
	return total;
}

//! Jobs in the order given. The do-nothing baseline: a search needs a
//! valid option that is usually poor, or every member looks equally good.
Sequence AsGiven(const vector<double> &m, idx_t n) {
	Sequence s;
	s.order.resize(n);
	std::iota(s.order.begin(), s.order.end(), 0);
	s.total_setup = TotalSetup(m, n, s.order);
	return s;
}

//! Repeatedly jump to the cheapest unvisited successor. Fast, and the
//! classic failure mode is stranding an expensive job until last.
Sequence NearestNeighbour(const vector<double> &m, idx_t n) {
	Sequence s;
	vector<bool> visited(n, false);
	idx_t current = 0;
	visited[0] = true;
	s.order.push_back(0);
	for (idx_t step = 1; step < n; step++) {
		idx_t best = n;
		double best_cost = 0;
		for (idx_t j = 0; j < n; j++) {
			if (visited[j]) {
				continue;
			}
			const double cost = m[current * n + j];
			if (best == n || cost < best_cost) {
				best = j;
				best_cost = cost;
			}
		}
		visited[best] = true;
		s.order.push_back(best);
		current = best;
	}
	s.total_setup = TotalSetup(m, n, s.order);
	return s;
}

//! Nearest-neighbour, then 2-opt: reverse any segment whose reversal
//! lowers total setup, until none does. This is the member that can
//! actually escape a bad greedy start.
Sequence TwoOpt(const vector<double> &m, idx_t n) {
	Sequence s = NearestNeighbour(m, n);
	bool improved = true;
	while (improved) {
		improved = false;
		for (idx_t i = 0; i + 1 < s.order.size() && !improved; i++) {
			for (idx_t k = i + 1; k < s.order.size(); k++) {
				auto candidate = s.order;
				std::reverse(candidate.begin() + static_cast<int64_t>(i),
				             candidate.begin() + static_cast<int64_t>(k) + 1);
				const double cost = TotalSetup(m, n, candidate);
				if (cost < s.total_setup - 1e-12) {
					s.order = std::move(candidate);
					s.total_setup = cost;
					improved = true;
					break;
				}
			}
		}
	}
	return s;
}

Sequence BestOfSequence(const vector<double> &m, idx_t n) {
	auto best = TwoOpt(m, n);
	for (auto candidate : {NearestNeighbour(m, n), AsGiven(m, n)}) {
		if (candidate.total_setup < best.total_setup) {
			best = candidate;
		}
	}
	return best;
}

} // namespace

static void AddSequencingFunction(ExtensionLoader &loader, const string &name,
                                  Sequence (*fn)(const vector<double> &, idx_t),
                                  const string &description, const string &example) {
	auto return_type = LogicalType::STRUCT({{"order", LogicalType::LIST(LogicalType::UBIGINT)},
	                                        {"total_setup", LogicalType::DOUBLE}});

	ScalarFunction function(
	    name, {LogicalType::LIST(LogicalType::DOUBLE), LogicalType::BIGINT}, return_type,
	    [fn, name](DataChunk &args, ExpressionState &, Vector &result) {
#ifdef ANOFOX_TELEMETRY_ENABLED
		    PostHogTelemetry::Instance().RecordFunctionCall(name);
#endif
		    const idx_t count = args.size();
		    result.SetVectorType(VectorType::FLAT_VECTOR);

		    UnifiedVectorFormat m_data, n_data;
		    args.data[0].ToUnifiedFormat(count, m_data);
		    args.data[1].ToUnifiedFormat(count, n_data);
		    auto m_lists = UnifiedVectorFormat::GetData<list_entry_t>(m_data);
		    auto n_vals = UnifiedVectorFormat::GetData<int64_t>(n_data);

		    auto &m_child = ListVector::GetEntry(args.data[0]);
		    UnifiedVectorFormat mc;
		    m_child.ToUnifiedFormat(ListVector::GetListSize(args.data[0]), mc);
		    auto m_vals = UnifiedVectorFormat::GetData<double>(mc);

		    auto &entries = StructVector::GetEntries(result);
		    auto &order_vec = *entries[0];
		    auto order_out = FlatVector::GetData<list_entry_t>(order_vec);
		    auto setup_out = FlatVector::GetData<double>(*entries[1]);
		    idx_t order_offset = 0;

		    for (idx_t row = 0; row < count; row++) {
			    const auto mi = m_data.sel->get_index(row);
			    const auto ni = n_data.sel->get_index(row);
			    if (!m_data.validity.RowIsValid(mi) || !n_data.validity.RowIsValid(ni)) {
				    FlatVector::SetNull(result, row, true);
				    order_out[row].offset = order_offset;
				    order_out[row].length = 0;
				    setup_out[row] = 0;
				    continue;
			    }
			    const int64_t n_signed = n_vals[ni];
			    if (n_signed < 0) {
				    throw InvalidInputException("job count must be >= 0, got %lld",
				                                static_cast<long long>(n_signed));
			    }
			    const idx_t n = static_cast<idx_t>(n_signed);
			    const auto entry = m_lists[mi];
			    if (entry.length != n * n) {
				    throw InvalidInputException(
				        "setup matrix has %llu entries but %llu jobs need exactly %llu "
				        "(an n*n matrix flattened row-major, setup[i*n+j] = cost of "
				        "running j immediately after i)",
				        static_cast<uint64_t>(entry.length), static_cast<uint64_t>(n),
				        static_cast<uint64_t>(n * n));
			    }

			    vector<double> m;
			    m.reserve(entry.length);
			    for (idx_t i = 0; i < entry.length; i++) {
				    const auto idx = mc.sel->get_index(entry.offset + i);
				    if (!mc.validity.RowIsValid(idx)) {
					    throw InvalidInputException(
					        "setup matrix entry %llu (row %llu, column %llu) is NULL — a "
					        "NULL cost cannot be compared, so that transition would be "
					        "silently unreachable",
					        static_cast<uint64_t>(i), static_cast<uint64_t>(i / (n ? n : 1)),
					        static_cast<uint64_t>(n ? i % n : 0));
				    }
				    const double v = m_vals[idx];
				    if (!std::isfinite(v)) {
					    throw InvalidInputException(
					        "setup matrix entry %llu is %f — costs must be finite; a "
					        "non-finite cost silently defeats every comparison",
					        static_cast<uint64_t>(i), v);
				    }
				    m.push_back(v);
			    }

			    const auto seq = n == 0 ? Sequence {} : fn(m, n);
			    setup_out[row] = seq.total_setup;
			    order_out[row].offset = order_offset;
			    order_out[row].length = seq.order.size();
			    ListVector::Reserve(order_vec, order_offset + seq.order.size());
			    auto order_child = FlatVector::GetData<uint64_t>(ListVector::GetEntry(order_vec));
			    for (idx_t i = 0; i < seq.order.size(); i++) {
				    order_child[order_offset + i] = seq.order[i];
			    }
			    order_offset += seq.order.size();
		    }
		    FlatVector::Validity(ListVector::GetEntry(order_vec)).SetAllValid(order_offset);
		    ListVector::SetListSize(order_vec, order_offset);
	    });

	FunctionDescription desc;
	desc.description = description;
	desc.examples.push_back(example);
	CreateScalarFunctionInfo info(function);
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

static void AddSequencingFamily(ExtensionLoader &loader, const string &short_name,
                                Sequence (*fn)(const vector<double> &, idx_t),
                                const string &description, const string &example) {
	AddSequencingFunction(loader, "anofox_optimize_" + short_name, fn, description,
	                      "anofox_optimize_" + example);
	AddSequencingFunction(loader, "opt_" + short_name, fn, description, "opt_" + example);
}

void RegisterSequencingFunctions(ExtensionLoader &loader) {
	const string matrix_note =
	    " `setup` is the n*n matrix flattened ROW-MAJOR: setup[i*n+j] is the cost of "
	    "running job j immediately after job i. Returns a 0-based job order and the "
	    "total setup cost of that order.";
	AddSequencingFamily(
	    loader, "sequence_as_given", AsGiven,
	    "Sequences jobs in the order given, changing nothing. The do-nothing baseline "
	    "and usually poor; included so a search has a valid option to move away from "
	    "rather than seeing every member look equally good." + matrix_note,
	    "sequence_as_given([0.0,5.0,9.0,5.0,0.0,3.0,9.0,3.0,0.0], 3)");
	AddSequencingFamily(
	    loader, "sequence_nearest_neighbour", NearestNeighbour,
	    "Sequences jobs by repeatedly jumping to the cheapest unvisited successor, "
	    "starting from job 0. Fast and usually far better than the given order; its "
	    "classic failure is stranding an expensive job until last, which it cannot "
	    "then undo." + matrix_note,
	    "sequence_nearest_neighbour([0.0,5.0,9.0,5.0,0.0,3.0,9.0,3.0,0.0], 3)");
	AddSequencingFamily(
	    loader, "sequence_two_opt", TwoOpt,
	    "Sequences jobs by nearest-neighbour and then improves with 2-OPT: reverses "
	    "any segment whose reversal lowers total setup, until none does. The member "
	    "that can escape a bad greedy start, at O(n^2) per improving pass." +
	        matrix_note,
	    "sequence_two_opt([0.0,5.0,9.0,5.0,0.0,3.0,9.0,3.0,0.0], 3)");
	AddSequencingFamily(
	    loader, "sequence_best_of", BestOfSequence,
	    "Runs every sequencing algorithm in this family and returns whichever had the "
	    "lowest total setup. Costs the sum of the parts and never loses to any single "
	    "member." + matrix_note,
	    "sequence_best_of([0.0,5.0,9.0,5.0,0.0,3.0,9.0,3.0,0.0], 3)");
}

} // namespace duckdb
