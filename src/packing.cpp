#include "packing.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/generic_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

#include "register_alias.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#ifdef ANOFOX_TELEMETRY_ENABLED
#include "telemetry.hpp"
#endif

#include <algorithm>
#include <cmath>
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
	// Explicit -> idx_t: `loads.size()` is size_type, which is the same
	// width as idx_t on Linux but NOT on macOS x86_64, where deducing two
	// different return types is a hard error. Caught by the first CI run.
	return FitPack(sizes, capacity, OrderBySizeDesc(sizes),
	               [](const vector<double> &loads, double size, double cap) -> idx_t {
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
	               [](const vector<double> &loads, double size, double cap) -> idx_t {
		               if (!loads.empty() && loads.back() + size <= cap) {
			               return loads.size() - 1;
		               }
		               return loads.size();
	               });
}

//! Try to empty the least-loaded bin by relocating its items into others,
//! including via a 1-1 swap that makes room. Repeats until no bin can be
//! emptied. This is what lets the family beat first-fit-decreasing on
//! triplet-style instances, where every greedy heuristic stalls at the
//! same answer — without an improvement member, "swap the algorithm"
//! changes the name and not the result.
Packing LocalSearch(const vector<double> &sizes, double capacity) {
	Packing best = FirstFitDecreasing(sizes, capacity);

	for (bool improved = true; improved;) {
		improved = false;
		const idx_t bin_count = best.bins_used;
		vector<vector<idx_t>> members(bin_count);
		vector<double> loads(bin_count, 0.0);
		for (idx_t i = 0; i < sizes.size(); i++) {
			members[best.assignment[i]].push_back(i);
			loads[best.assignment[i]] += sizes[i];
		}

		// Try to dissolve EVERY bin, emptiest first — not just the
		// emptiest one. Stopping after a single failed target was enough
		// to leave a triplet instance at first-fit-decreasing's answer:
		// the emptiest bin is often the one holding the awkward item,
		// while a different bin dissolves easily.
		vector<idx_t> targets(bin_count);
		std::iota(targets.begin(), targets.end(), 0);
		std::stable_sort(targets.begin(), targets.end(),
		                 [&](idx_t a, idx_t b) { return loads[a] < loads[b]; });

		vector<idx_t> trial_assignment;
		bool relocated_all = false;
		idx_t target = 0;
		for (auto candidate_target : targets) {
			target = candidate_target;
			trial_assignment = best.assignment;
			auto trial_loads = loads;
			relocated_all = true;
			for (auto item : members[target]) {
				bool placed = false;
				for (idx_t b = 0; b < bin_count && !placed; b++) {
					if (b == target) {
						continue;
					}
					if (trial_loads[b] + sizes[item] <= capacity) {
						trial_loads[b] += sizes[item];
						trial_assignment[item] = b;
						placed = true;
					}
				}
				// No direct home: swap with a smaller item in bin b, but
				// that displaced item must go to a THIRD bin — never back
				// into the bin being dissolved. Putting it there left the
				// target non-empty while the renumbering below removed it
				// anyway, silently merging two bins and returning an
				// OVER-CAPACITY packing (it reported 6 bins where 10 is
				// the proven minimum). Caught by the feasibility test.
				for (idx_t b = 0; b < bin_count && !placed; b++) {
					if (b == target) {
						continue;
					}
					for (idx_t other = 0; other < sizes.size() && !placed; other++) {
						if (trial_assignment[other] != b || sizes[other] >= sizes[item]) {
							continue;
						}
						if (trial_loads[b] - sizes[other] + sizes[item] > capacity) {
							continue;
						}
						for (idx_t c = 0; c < bin_count; c++) {
							if (c == target || c == b) {
								continue;
							}
							if (trial_loads[c] + sizes[other] <= capacity) {
								trial_loads[b] += sizes[item] - sizes[other];
								trial_loads[c] += sizes[other];
								trial_loads[target] -= sizes[item];
								trial_assignment[item] = b;
								trial_assignment[other] = c;
								placed = true;
								break;
							}
						}
					}
				}
				if (!placed) {
					relocated_all = false;
					break;
				}
			}
			if (relocated_all) {
				break;
			}
		}

		if (relocated_all) {
			// The target bin is empty; renumber to close the gap.
			vector<idx_t> remap(bin_count, 0);
			idx_t next = 0;
			for (idx_t b = 0; b < bin_count; b++) {
				remap[b] = (b == target) ? 0 : next++;
			}
			for (auto &a : trial_assignment) {
				a = remap[a];
			}
			best.assignment = trial_assignment;
			best.bins_used = next;
			improved = true;
		}
	}
	return best;
}

//! Bounded bin completion: fill each bin by searching COMPANIONS for a
//! seed item, rather than taking items one at a time.
//!
//! This is NOT Korf's bin completion and NOT exhaustive over arbitrary
//! subsets: it seeds with the largest unplaced item, searches all PAIRS
//! of companions exhaustively, and then tops up greedily with anything
//! that still fits. Enough to recover exact three-per-bin fills, which
//! is what defeats the greedy members; not enough to claim optimality.
//!
//! The greedy members all commit to a locally-good first item and then
//! cannot recover; on triplet-style instances, where the optimum packs
//! an exact-fitting subset into every bin, they all stall at the same
//! answer. Searching subsets up to `max_subset` finds those exact fills.
//!
//! `max_subset` bounds the search: subsets are examined over the largest
//! remaining items, so cost is O(n^max_subset) per bin in the worst case.
//! 3 recovers classic triplet instances; higher costs more and rarely
//! helps.
Packing BinCompletion(const vector<double> &sizes, double capacity, idx_t max_subset) {
	const idx_t n = sizes.size();
	Packing result;
	result.assignment.assign(n, 0);
	vector<bool> used(n, false);
	auto order = OrderBySizeDesc(sizes);
	idx_t placed_count = 0;
	idx_t bin = 0;

	while (placed_count < n) {
		// Seed the bin with the largest unplaced item: it is the hardest
		// to place later, so it should choose its companions.
		idx_t seed = n;
		for (auto i : order) {
			if (!used[i]) {
				seed = i;
				break;
			}
		}
		vector<idx_t> chosen {seed};
		double best_total = sizes[seed];

		// EXHAUSTIVE over companions, not greedy per step. Choosing the
		// single best next item at each step still stalls on triplets:
		// the addition that gets closest to full on its own can leave no
		// feasible completion. Enumerating pairs finds the exact fill.
		vector<idx_t> remaining;
		for (auto i : order) {
			if (!used[i] && i != seed) {
				remaining.push_back(i);
			}
		}
		if (max_subset >= 3) {
			double best_pair_total = best_total;
			idx_t best_a = n, best_b = n;
			for (idx_t x = 0; x < remaining.size(); x++) {
				const double with_x = sizes[seed] + sizes[remaining[x]];
				if (with_x > capacity) {
					continue;
				}
				if (with_x > best_pair_total) {
					best_pair_total = with_x;
					best_a = remaining[x];
					best_b = n;
				}
				for (idx_t y = x + 1; y < remaining.size(); y++) {
					const double total = with_x + sizes[remaining[y]];
					if (total <= capacity && total > best_pair_total) {
						best_pair_total = total;
						best_a = remaining[x];
						best_b = remaining[y];
					}
				}
			}
			if (best_a != n) {
				chosen.push_back(best_a);
			}
			if (best_b != n) {
				chosen.push_back(best_b);
			}
			best_total = best_pair_total;
		} else if (max_subset == 2 && !remaining.empty()) {
			idx_t best_one = n;
			double best_one_total = best_total;
			for (auto i : remaining) {
				const double total = best_total + sizes[i];
				if (total <= capacity && total > best_one_total) {
					best_one_total = total;
					best_one = i;
				}
			}
			if (best_one != n) {
				chosen.push_back(best_one);
				best_total = best_one_total;
			}
		}

		// Top up with anything else that still fits.
		for (auto i : order) {
			if (used[i] || std::find(chosen.begin(), chosen.end(), i) != chosen.end()) {
				continue;
			}
			if (best_total + sizes[i] <= capacity) {
				chosen.push_back(i);
				best_total += sizes[i];
			}
		}

		for (auto i : chosen) {
			used[i] = true;
			result.assignment[i] = bin;
			placed_count++;
		}
		bin++;
	}
	result.bins_used = bin;
	return result;
}

Packing BinCompletion3(const vector<double> &sizes, double capacity) {
	return BinCompletion(sizes, capacity, 3);
}

//! Run every member of the family and keep whichever used fewest bins.
//! Costs the sum of the parts and never loses to any single member.
Packing BestOf(const vector<double> &sizes, double capacity) {
	Packing best = LocalSearch(sizes, capacity);
	for (auto candidate : {BinCompletion3(sizes, capacity),
	                       FirstFitDecreasing(sizes, capacity),
	                       BestFitDecreasing(sizes, capacity),
	                       WorstFitDecreasing(sizes, capacity),
	                       NextFit(sizes, capacity)}) {
		if (candidate.bins_used < best.bins_used) {
			best = candidate;
		}
	}
	return best;
}

} // namespace

//! Register one packing function. `fn` is the algorithm; everything else
//! is identical across the family so the signatures match exactly.
static void AddPackingFunction(ExtensionLoader &loader, const string &name,
                               Packing (*fn)(const vector<double> &, double),
                               const string &description, const string &example, const string &alias_of) {
	auto return_type =
	    LogicalType::STRUCT({{"bins_used", LogicalType::UBIGINT},
	                         {"assignment", LogicalType::LIST(LogicalType::UBIGINT)}});

	ScalarFunction function(
	    name, {LogicalType::LIST(LogicalType::DOUBLE), LogicalType::DOUBLE}, return_type,
	    [fn, name](DataChunk &args, ExpressionState &, Vector &result) {
#ifdef ANOFOX_TELEMETRY_ENABLED
		    PostHogTelemetry::Instance().RecordFunctionCall(name);
#endif
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
				    // Leave no garbage behind the NULL: an uninitialised
				    // list_entry_t carries an arbitrary offset/length that a
				    // consumer reaching past the validity mask would follow.
				    assign_out[row].offset = assign_offset;
				    assign_out[row].length = 0;
				    bins_out[row] = 0;
				    continue;
			    }
			    const double capacity = caps[cidx];
			    if (!std::isfinite(capacity)) {
				    throw InvalidInputException(
				        "capacity must be finite, got %f — a non-finite capacity makes "
				        "every fit test meaningless",
				        capacity);
			    }
			    if (!(capacity > 0)) {
				    throw InvalidInputException(
				        "capacity must be > 0, got %f — no item can be placed in a bin "
				        "of non-positive capacity",
				        capacity);
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
				    // Reject before packing. Both of these previously slipped
				    // through and returned a confident WRONG answer: a negative
				    // size reported 1 bin for [-5, 3], and NaN reported 2 for
				    // [NaN, 3] because every comparison against NaN is false, so
				    // it passed the capacity check and then fitted nowhere.
				    if (!std::isfinite(size)) {
					    throw InvalidInputException(
					        "item %llu has size %f — sizes must be finite; a non-finite "
					        "size silently defeats every capacity check",
					        static_cast<uint64_t>(i), size);
				    }
				    if (size < 0) {
					    throw InvalidInputException(
					        "item %llu has size %f — sizes must be >= 0; a negative size "
					        "would let items 'free up' space that does not exist",
					        static_cast<uint64_t>(i), size);
				    }
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
		    // The child values were all written explicitly, but a REUSED
		    // child vector can carry stale null validity from a previous
		    // chunk — the assignment would then read back as NULLs even
		    // though every slot holds a real bin id. Mark the written
		    // range valid. (Codex review finding, High.)
		    FlatVector::Validity(ListVector::GetEntry(assign_vec)).SetAllValid(assign_offset);
		    ListVector::SetListSize(assign_vec, assign_offset);
	    });

	RegisterScalarOrAlias(loader, std::move(function), description, example, alias_of);
}

//! Register a packing algorithm under BOTH the canonical
//! `anofox_optimize_*` name and the short `opt_*` alias, matching
//! anofox-statistics (`anofox_stats_aic` + `aic`). The canonical name
//! keeps the catalog unambiguous when several anofox extensions are
//! loaded together; the short one is what a policy actually types.
static void AddPackingFamily(ExtensionLoader &loader, const string &short_name,
                             Packing (*fn)(const vector<double> &, double),
                             const string &description, const string &example) {
	const string canonical = "anofox_optimize_" + short_name;
	AddPackingFunction(loader, canonical, fn, description, "anofox_optimize_" + example, "");
	AddPackingFunction(loader, "opt_" + short_name, fn, description, "opt_" + example, canonical);
}

void RegisterPackingFunctions(ExtensionLoader &loader) {
	AddPackingFamily(
	    loader, "pack_first_fit_decreasing", FirstFitDecreasing,
	    "Packs items into bins by first-fit-decreasing: sorts items largest first and "
	    "puts each into the first bin it fits. Fast and near-optimal on typical "
	    "instances, but provably up to 11/9 of optimal and weak on triplet-style "
	    "instances. Returns bins_used and a 0-based bin index per input item. An "
	    "empty item list uses 0 bins; a non-empty list of zero-size items uses 1.",
	    "pack_first_fit_decreasing([4.0, 8.0, 1.0], 10.0)");
	AddPackingFamily(
	    loader, "pack_best_fit_decreasing", BestFitDecreasing,
	    "Packs items into bins by best-fit-decreasing: sorts items largest first and "
	    "puts each into the FULLEST bin it still fits, leaving emptier bins for larger "
	    "items later. Same signature as the other packing functions.",
	    "pack_best_fit_decreasing([4.0, 8.0, 1.0], 10.0)");
	AddPackingFamily(
	    loader, "pack_worst_fit_decreasing", WorstFitDecreasing,
	    "Packs items into bins by worst-fit-decreasing: puts each item into the "
	    "EMPTIEST bin it fits, spreading load evenly. Usually uses more bins than "
	    "best-fit but produces balanced loads. Same signature as the other packing "
	    "functions.",
	    "pack_worst_fit_decreasing([4.0, 8.0, 1.0], 10.0)");
	AddPackingFamily(
	    loader, "pack_next_fit", NextFit,
	    "Packs items into bins by next-fit, in the given order, only ever using the "
	    "most recently opened bin. The weakest member of the family and the cheapest; "
	    "included so a search has a poor-but-valid baseline to move away from.",
	    "pack_next_fit([4.0, 8.0, 1.0], 10.0)");
	AddPackingFamily(
	    loader, "pack_local_search", LocalSearch,
	    "Packs items by first-fit-decreasing and then IMPROVES the result: tries to "
	    "dissolve bins one at a time, from least-loaded upward, by relocating their "
	    "items into other bins — including a swap that displaces a smaller item to a "
	    "third bin to make room. Repeats until no bin can be emptied. Slower than the "
	    "greedy members; on triplet-style instances it does not beat them (use "
	    "bin_completion there). Same signature as the other packing functions.",
	    "pack_local_search([4.0, 8.0, 1.0], 10.0)");
	AddPackingFamily(
	    loader, "pack_bin_completion", BinCompletion3,
	    "Packs items by BOUNDED BIN COMPLETION: seeds each bin with the largest "
	    "unplaced item, searches all PAIRS of companions exhaustively for the "
	    "combination that fills the bin fullest, then tops up greedily with anything "
	    "that still fits. Not exhaustive over arbitrary subsets and not optimal, but "
	    "it recovers the exact three-per-bin fills that make every greedy member "
	    "stall together on triplet-style instances. Costs O(n^2) per bin. Same "
	    "signature as the other packing functions.",
	    "pack_bin_completion([4.0, 8.0, 1.0], 10.0)");
	AddPackingFamily(
	    loader, "pack_best_of", BestOf,
	    "Runs every packing algorithm in this family and returns whichever used the "
	    "fewest bins. Costs the sum of the parts and never loses to any single "
	    "member; use it when you do not want to choose. Same signature as the other "
	    "packing functions.",
	    "pack_best_of([4.0, 8.0, 1.0], 10.0)");
}

} // namespace duckdb
