#include "batching.hpp"

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

struct Waves {
	vector<idx_t> wave; // 1-based wave number per order
	double avg_weighted_wave = 0;
};

//! Pack orders into capacity-bounded waves in the given index order.
Waves PackInOrder(const vector<double> &items, const vector<double> &priority, double capacity,
                  const vector<idx_t> &order) {
	Waves w;
	w.wave.assign(items.size(), 1);
	vector<double> loads;
	for (auto o : order) {
		idx_t placed = loads.size();
		for (idx_t b = 0; b < loads.size(); b++) {
			if (loads[b] + items[o] <= capacity) {
				placed = b;
				break;
			}
		}
		if (placed == loads.size()) {
			loads.push_back(items[o]);
		} else {
			loads[placed] += items[o];
		}
		w.wave[o] = placed + 1;
	}
	double total = 0, weight = 0;
	for (idx_t i = 0; i < items.size(); i++) {
		total += static_cast<double>(w.wave[i]) * priority[i];
		weight += priority[i];
	}
	w.avg_weighted_wave = weight > 0 ? total / weight : 0.0;
	return w;
}

vector<idx_t> Identity(idx_t n) {
	vector<idx_t> o(n);
	std::iota(o.begin(), o.end(), 0);
	return o;
}

//! Given order. The weak baseline a search needs to move away from.
Waves AsGiven(const vector<double> &items, const vector<double> &priority, double capacity) {
	return PackInOrder(items, priority, capacity, Identity(items.size()));
}

//! Highest priority first, so urgent orders land in early waves. This is
//! what the objective actually rewards.
Waves PriorityFirst(const vector<double> &items, const vector<double> &priority, double capacity) {
	auto order = Identity(items.size());
	std::stable_sort(order.begin(), order.end(),
	                 [&](idx_t a, idx_t b) { return priority[a] > priority[b]; });
	return PackInOrder(items, priority, capacity, order);
}

//! Highest priority PER UNIT OF WORK first: an urgent order that fills a
//! whole wave delays everything behind it.
Waves PriorityDensity(const vector<double> &items, const vector<double> &priority,
                      double capacity) {
	auto order = Identity(items.size());
	std::stable_sort(order.begin(), order.end(), [&](idx_t a, idx_t b) {
		const double ra = items[a] > 0 ? priority[a] / items[a] : std::numeric_limits<double>::max();
		const double rb = items[b] > 0 ? priority[b] / items[b] : std::numeric_limits<double>::max();
		return ra > rb;
	});
	return PackInOrder(items, priority, capacity, order);
}

//! Fewest waves first (largest-first packing), then priority within.
//! Optimising wave COUNT rather than weighted wave — deliberately kept
//! so the family shows what the wrong objective costs.
Waves FewestWaves(const vector<double> &items, const vector<double> &priority, double capacity) {
	auto order = Identity(items.size());
	std::stable_sort(order.begin(), order.end(),
	                 [&](idx_t a, idx_t b) { return items[a] > items[b]; });
	return PackInOrder(items, priority, capacity, order);
}

Waves BestOfWaves(const vector<double> &items, const vector<double> &priority, double capacity) {
	auto best = PriorityFirst(items, priority, capacity);
	for (auto c : {PriorityDensity(items, priority, capacity), FewestWaves(items, priority, capacity),
	               AsGiven(items, priority, capacity)}) {
		if (c.avg_weighted_wave < best.avg_weighted_wave) {
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
		throw InvalidInputException(
		    "%s has %llu entries but %llu were required — every order needs exactly one",
		    what, static_cast<uint64_t>(entry.length), static_cast<uint64_t>(expect));
	}
	vector<double> out;
	out.reserve(entry.length);
	for (idx_t i = 0; i < entry.length; i++) {
		const auto ci = cd.sel->get_index(entry.offset + i);
		if (!cd.validity.RowIsValid(ci)) {
			throw InvalidInputException("%s entry %llu is NULL — a NULL cannot be compared, so "
			                            "that order would be silently unbatchable",
			                            what, static_cast<uint64_t>(i));
		}
		const double x = values[ci];
		if (!std::isfinite(x)) {
			throw InvalidInputException("%s entry %llu is %f — values must be finite", what,
			                            static_cast<uint64_t>(i), x);
		}
		if (x < 0) {
			throw InvalidInputException("%s entry %llu is %f — must be >= 0", what,
			                            static_cast<uint64_t>(i), x);
		}
		out.push_back(x);
	}
	return out;
}

} // namespace

static void AddBatchingFunction(ExtensionLoader &loader, const string &name,
                                Waves (*fn)(const vector<double> &, const vector<double> &, double),
                                const string &description, const string &example) {
	auto return_type = LogicalType::STRUCT({{"wave", LogicalType::LIST(LogicalType::UBIGINT)},
	                                        {"avg_weighted_wave", LogicalType::DOUBLE}});
	ScalarFunction function(
	    name,
	    {LogicalType::LIST(LogicalType::DOUBLE), LogicalType::LIST(LogicalType::DOUBLE),
	     LogicalType::DOUBLE},
	    return_type, [fn, name](DataChunk &args, ExpressionState &, Vector &result) {
#ifdef ANOFOX_TELEMETRY_ENABLED
		    PostHogTelemetry::Instance().RecordFunctionCall(name);
#endif
		    const idx_t count = args.size();
		    result.SetVectorType(VectorType::FLAT_VECTOR);
		    UnifiedVectorFormat c_data;
		    args.data[2].ToUnifiedFormat(count, c_data);
		    auto caps = UnifiedVectorFormat::GetData<double>(c_data);

		    auto &entries = StructVector::GetEntries(result);
		    auto &wave_vec = *entries[0];
		    auto wave_out = FlatVector::GetData<list_entry_t>(wave_vec);
		    auto avg_out = FlatVector::GetData<double>(*entries[1]);
		    idx_t offset = 0;

		    for (idx_t row = 0; row < count; row++) {
			    const auto ci = c_data.sel->get_index(row);
			    if (!c_data.validity.RowIsValid(ci)) {
				    FlatVector::SetNull(result, row, true);
				    wave_out[row].offset = offset;
				    wave_out[row].length = 0;
				    avg_out[row] = 0;
				    continue;
			    }
			    const double capacity = caps[ci];
			    if (!std::isfinite(capacity) || capacity <= 0) {
				    throw InvalidInputException(
				        "wave capacity must be finite and > 0, got %f — no order can be "
				        "batched into a wave of non-positive capacity",
				        capacity);
			    }
			    auto items = ReadDoubles(args.data[0], count, row, "items", 0);
			    auto priority = ReadDoubles(args.data[1], count, row, "priorities", items.size());
			    for (idx_t i = 0; i < items.size(); i++) {
				    if (items[i] > capacity) {
					    throw InvalidInputException(
					        "order %llu needs %f units but a wave holds %f — it cannot be "
					        "batched at all",
					        static_cast<uint64_t>(i), items[i], capacity);
				    }
			    }

			    const auto w = items.empty() ? Waves {} : fn(items, priority, capacity);
			    avg_out[row] = w.avg_weighted_wave;
			    wave_out[row].offset = offset;
			    wave_out[row].length = w.wave.size();
			    ListVector::Reserve(wave_vec, offset + w.wave.size());
			    auto child = FlatVector::GetData<uint64_t>(ListVector::GetEntry(wave_vec));
			    for (idx_t i = 0; i < w.wave.size(); i++) {
				    child[offset + i] = w.wave[i];
			    }
			    offset += w.wave.size();
		    }
		    FlatVector::Validity(ListVector::GetEntry(wave_vec)).SetAllValid(offset);
		    ListVector::SetListSize(wave_vec, offset);
	    });

	FunctionDescription desc;
	desc.description = description;
	desc.examples.push_back(example);
	CreateScalarFunctionInfo info(function);
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

static void AddBatchingFamily(ExtensionLoader &loader, const string &short_name,
                              Waves (*fn)(const vector<double> &, const vector<double> &, double),
                              const string &description, const string &example) {
	AddBatchingFunction(loader, "anofox_optimize_" + short_name, fn, description,
	                    "anofox_optimize_" + example);
	AddBatchingFunction(loader, "opt_" + short_name, fn, description, "opt_" + example);
}

void RegisterBatchingFunctions(ExtensionLoader &loader) {
	const string shape =
	    " Takes items and priorities (one per order) and a per-wave capacity. Returns a "
	    "1-based wave number per order and the priority-weighted mean wave, "
	    "sum(wave*priority)/sum(priority) — lower is better.";
	const string ex = "([3.0,4.0,5.0],[1.0,5.0,1.0], 7.0)";

	AddBatchingFamily(loader, "wave_as_given", AsGiven,
	                  "Batches orders into capacity-bounded waves in the order given. The "
	                  "do-nothing baseline and usually poor; included so a search has a "
	                  "valid option to move away from." + shape,
	                  "wave_as_given" + ex);
	AddBatchingFamily(loader, "wave_priority_first", PriorityFirst,
	                  "Batches HIGHEST PRIORITY FIRST, so urgent orders land in early waves. "
	                  "Directly targets the weighted-wave objective; can waste capacity when "
	                  "an urgent order is also large." + shape,
	                  "wave_priority_first" + ex);
	AddBatchingFamily(loader, "wave_priority_density", PriorityDensity,
	                  "Batches by highest priority PER UNIT OF WORK, since an urgent order "
	                  "that fills a whole wave delays everything behind it. Usually beats "
	                  "plain priority-first when order sizes vary widely." + shape,
	                  "wave_priority_density" + ex);
	AddBatchingFamily(loader, "wave_fewest_waves", FewestWaves,
	                  "Batches largest orders first, minimising the NUMBER of waves rather "
	                  "than the weighted mean wave. Kept deliberately: it optimises the "
	                  "wrong quantity for this objective, and shows what that costs." + shape,
	                  "wave_fewest_waves" + ex);
	AddBatchingFamily(loader, "wave_best_of", BestOfWaves,
	                  "Runs every batching algorithm in this family and returns whichever "
	                  "had the lowest priority-weighted mean wave." + shape,
	                  "wave_best_of" + ex);
}

} // namespace duckdb
