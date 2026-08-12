#include "packing.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/generic_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <algorithm>
#include <numeric>
#include <vector>

namespace duckdb {

namespace {

//! One packing: which bin each item went into, and how many bins were used.
struct Packing {
	vector<idx_t> assignment;
	idx_t bins_used = 0;
};

//! Order item indices by descending size — the "decreasing" in FFD/BFD/WFD.
vector<idx_t> OrderBySizeDesc(const vector<double> &sizes) {
	vector<idx_t> order(sizes.size());
	std::iota(order.begin(), order.end(), 0);
	std::stable_sort(order.begin(), order.end(),
	                 [&](idx_t a, idx_t b) { return sizes[a] > sizes[b]; });
	return order;
}

//! Shared skeleton for the fit heuristics. `choose` picks which open bin to
//! use given the current loads, or returns bins.size() to open a new one.
//! The families differ ONLY in that choice, which is exactly why they are
//! worth exposing as separate, swappable functions.
template <typename Choose>
Packing FitPack(const vector<double> &sizes, double capacity,
                const vector<idx_t> &order, Choose choose) {
	Packing result;
	result.assignment.assign(sizes.size(), 0);
	vector<double> loads;
	for (auto item : order) {
		const double size = sizes[item];
		const idx_t bin = choose(loads, size, capacity);
		if (bin == loads.size()) {
			loads.push_back(size);
		} else {
			loads[bin] += size;
		}
		result.assignment[item] = bin;
	}
	result.bins_used = loads.size();
	return result;
}

Packing FirstFitDecreasing(const vector<double> &sizes, double capacity) {
	return FitPack(sizes, capacity, OrderBySizeDesc(sizes),
	               [](const vector<double> &loads, double size, double cap) {
		               for (idx_t i = 0; i < loads.size(); i++) {
			               if (loads[i] + size <= cap) {
				               return i;
			               }
		               }
		               return loads.size();
	               });
}

Packing BestFitDecreasing(const vector<double> &sizes, double capacity) {
	return FitPack(sizes, capacity, OrderBySizeDesc(sizes),
	               [](const vector<double> &loads, double size, double cap) {
		               idx_t best = loads.size();
		               double tightest = -1;
		               for (idx_t i = 0; i < loads.size(); i++) {
			               if (loads[i] + size <= cap && loads[i] > tightest) {
				               tightest = loads[i];
				               best = i;
			               }
		               }
		               return best;
	               });
}

Packing WorstFitDecreasing(const vector<double> &sizes, double capacity) {
	return FitPack(sizes, capacity, OrderBySizeDesc(sizes),
	               [](const vector<double> &loads, double size, double cap) {
		               idx_t best = loads.size();
		               double emptiest = std::numeric_limits<double>::max();
		               for (idx_t i = 0; i < loads.size(); i++) {
			               if (loads[i] + size <= cap && loads[i] < emptiest) {
				               emptiest = loads[i];
				               best = i;
			               }
		               }
		               return best;
	               });
}

//! Next-fit: only ever the most recently opened bin. Deliberately weak —
//! a search needs a bad-but-valid member of the family to move away from.
Packing NextFit(const vector<double> &sizes, double capacity) {
	vector<idx_t> order(sizes.size());
	std::iota(order.begin(), order.end(), 0);
	return FitPack(sizes, capacity, order,
	               [](const vector<double> &loads, double size, double cap) {
		               if (!loads.empty() && loads.back() + size <= cap) {
			               return loads.size() - 1;
		               }
		               return loads.size();
	               });
}

} // namespace

//! Register one packing function. `fn` is the algorithm; everything else
//! is identical across the family so the signatures match exactly.
static void AddPackingFunction(ExtensionLoader &loader, const string &name,
                               Packing (*fn)(const vector<double> &, double),
                               const string &description, const string &example) {
	auto return_type =
	    LogicalType::STRUCT({{"bins_used", LogicalType::UBIGINT},
	                         {"assignment", LogicalType::LIST(LogicalType::UBIGINT)}});

	ScalarFunction function(
	    name, {LogicalType::LIST(LogicalType::DOUBLE), LogicalType::DOUBLE}, return_type,
	    [fn](DataChunk &args, ExpressionState &, Vector &result) {
		    const idx_t count = args.size();
		    result.SetVectorType(VectorType::FLAT_VECTOR);

		    UnifiedVectorFormat list_data, cap_data;
		    args.data[0].ToUnifiedFormat(count, list_data);
		    args.data[1].ToUnifiedFormat(count, cap_data);
		    auto lists = UnifiedVectorFormat::GetData<list_entry_t>(list_data);
		    auto caps = UnifiedVectorFormat::GetData<double>(cap_data);

		    auto &child = ListVector::GetEntry(args.data[0]);
		    UnifiedVectorFormat child_data;
		    child.ToUnifiedFormat(ListVector::GetListSize(args.data[0]), child_data);
		    auto child_values = UnifiedVectorFormat::GetData<double>(child_data);

		    auto &struct_entries = StructVector::GetEntries(result);
		    auto bins_out = FlatVector::GetData<uint64_t>(*struct_entries[0]);
		    auto &assign_vec = *struct_entries[1];
		    auto assign_out = FlatVector::GetData<list_entry_t>(assign_vec);
		    idx_t assign_offset = 0;

		    for (idx_t row = 0; row < count; row++) {
			    const auto lidx = list_data.sel->get_index(row);
			    const auto cidx = cap_data.sel->get_index(row);
			    if (!list_data.validity.RowIsValid(lidx) ||
			        !cap_data.validity.RowIsValid(cidx)) {
				    FlatVector::SetNull(result, row, true);
				    continue;
			    }
			    const double capacity = caps[cidx];
			    if (!(capacity > 0)) {
				    throw InvalidInputException("capacity must be > 0, got %f", capacity);
			    }

			    const auto entry = lists[lidx];
			    vector<double> sizes;
			    sizes.reserve(entry.length);
			    for (idx_t i = 0; i < entry.length; i++) {
				    const auto ci = child_data.sel->get_index(entry.offset + i);
				    if (!child_data.validity.RowIsValid(ci)) {
					    throw InvalidInputException("sizes must not contain NULL");
				    }
				    const double size = child_values[ci];
				    if (size > capacity) {
					    throw InvalidInputException(
					        "item of size %f exceeds capacity %f — no packing exists", size,
					        capacity);
				    }
				    sizes.push_back(size);
			    }

			    const auto packing = fn(sizes, capacity);
			    bins_out[row] = packing.bins_used;
			    assign_out[row].offset = assign_offset;
			    assign_out[row].length = packing.assignment.size();
			    ListVector::Reserve(assign_vec, assign_offset + packing.assignment.size());
			    auto assign_child = FlatVector::GetData<uint64_t>(ListVector::GetEntry(assign_vec));
			    for (idx_t i = 0; i < packing.assignment.size(); i++) {
				    assign_child[assign_offset + i] = packing.assignment[i];
			    }
			    assign_offset += packing.assignment.size();
		    }
		    ListVector::SetListSize(assign_vec, assign_offset);
	    });

	FunctionDescription desc;
	desc.description = description;
	desc.examples.push_back(example);
	CreateScalarFunctionInfo info(function);
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

void RegisterPackingFunctions(ExtensionLoader &loader) {
	AddPackingFunction(
	    loader, "opt_pack_first_fit_decreasing", FirstFitDecreasing,
	    "Packs items into bins by first-fit-decreasing: sorts items largest first and "
	    "puts each into the first bin it fits. Fast and near-optimal on typical "
	    "instances, but provably up to 11/9 of optimal and weak on triplet-style "
	    "instances. Returns bins_used and a 0-based bin index per input item.",
	    "opt_pack_first_fit_decreasing([4.0, 8.0, 1.0], 10.0)");
	AddPackingFunction(
	    loader, "opt_pack_best_fit_decreasing", BestFitDecreasing,
	    "Packs items into bins by best-fit-decreasing: sorts items largest first and "
	    "puts each into the FULLEST bin it still fits, leaving emptier bins for larger "
	    "items later. Same signature as the other packing functions.",
	    "opt_pack_best_fit_decreasing([4.0, 8.0, 1.0], 10.0)");
	AddPackingFunction(
	    loader, "opt_pack_worst_fit_decreasing", WorstFitDecreasing,
	    "Packs items into bins by worst-fit-decreasing: puts each item into the "
	    "EMPTIEST bin it fits, spreading load evenly. Usually uses more bins than "
	    "best-fit but produces balanced loads. Same signature as the other packing "
	    "functions.",
	    "opt_pack_worst_fit_decreasing([4.0, 8.0, 1.0], 10.0)");
	AddPackingFunction(
	    loader, "opt_pack_next_fit", NextFit,
	    "Packs items into bins by next-fit, in the given order, only ever using the "
	    "most recently opened bin. The weakest member of the family and the cheapest; "
	    "included so a search has a poor-but-valid baseline to move away from.",
	    "opt_pack_next_fit([4.0, 8.0, 1.0], 10.0)");
}

} // namespace duckdb
