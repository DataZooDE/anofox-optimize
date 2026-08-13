#include "scheduling.hpp"

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
#include <limits>
#include <numeric>
#include <vector>

namespace duckdb {

namespace {

struct Schedule {
	vector<idx_t> order;
	double total_weighted_tardiness = 0;
	double makespan = 0;
};

struct Instance {
	vector<double> processing, due, priority, setup;
	idx_t n = 0;
	//! setup[from*(n+1)+to] with index 0 meaning "the virtual start", so
	//! the first job's setup is charged like every other transition.
	double SetupCost(idx_t from, idx_t to) const {
		return setup[from * (n + 1) + to];
	}
};

//! Score an order: walk it, charging setup then processing, and
//! accumulate priority-weighted lateness.
Schedule Evaluate(const Instance &inst, vector<idx_t> order) {
	Schedule s;
	s.order = std::move(order);
	double clock = 0;
	idx_t previous = 0; // virtual start
	for (auto job : s.order) {
		clock += inst.SetupCost(previous, job + 1) + inst.processing[job];
		const double lateness = clock - inst.due[job];
		if (lateness > 0) {
			s.total_weighted_tardiness += inst.priority[job] * lateness;
		}
		previous = job + 1;
	}
	s.makespan = clock;
	return s;
}

//! Earliest due date. The textbook rule; optimal for maximum lateness
//! with no setups, and it ignores both priority and setup entirely.
Schedule EarliestDueDate(const Instance &inst) {
	vector<idx_t> order(inst.n);
	std::iota(order.begin(), order.end(), 0);
	std::stable_sort(order.begin(), order.end(),
	                 [&](idx_t a, idx_t b) { return inst.due[a] < inst.due[b]; });
	return Evaluate(inst, std::move(order));
}

//! Weighted shortest processing time. Ignores due dates, so it is strong
//! when everything is late and weak when few things are.
Schedule WeightedShortestProcessing(const Instance &inst) {
	vector<idx_t> order(inst.n);
	std::iota(order.begin(), order.end(), 0);
	std::stable_sort(order.begin(), order.end(), [&](idx_t a, idx_t b) {
		const double ra = inst.processing[a] > 0 ? inst.priority[a] / inst.processing[a]
		                                         : std::numeric_limits<double>::max();
		const double rb = inst.processing[b] > 0 ? inst.priority[b] / inst.processing[b]
		                                         : std::numeric_limits<double>::max();
		return ra > rb;
	});
	return Evaluate(inst, std::move(order));
}

//! Apparent Tardiness Cost with setups (ATCS): at each step pick the job
//! maximising priority/processing * exp(-slack/k1) * exp(-setup/k2).
//! Unlike EDD and WSPT it weighs due dates, priorities AND setups
//! together, which is what this objective actually trades off.
Schedule ApparentTardinessCost(const Instance &inst) {
	const double avg_p =
	    inst.n ? std::accumulate(inst.processing.begin(), inst.processing.end(), 0.0) / inst.n : 1.0;
	const double avg_s =
	    inst.setup.empty()
	        ? 1.0
	        : std::max(1e-9, std::accumulate(inst.setup.begin(), inst.setup.end(), 0.0) /
	                             static_cast<double>(inst.setup.size()));
	const double k1 = 2.0, k2 = 2.0;

	vector<bool> done(inst.n, false);
	vector<idx_t> order;
	double clock = 0;
	idx_t previous = 0;
	for (idx_t step = 0; step < inst.n; step++) {
		idx_t best = inst.n;
		double best_index = -std::numeric_limits<double>::max();
		for (idx_t j = 0; j < inst.n; j++) {
			if (done[j]) {
				continue;
			}
			const double p = std::max(1e-9, inst.processing[j]);
			const double slack = std::max(0.0, inst.due[j] - p - clock);
			const double setup = inst.SetupCost(previous, j + 1);
			const double index = (inst.priority[j] / p) * std::exp(-slack / (k1 * avg_p)) *
			                     std::exp(-setup / (k2 * avg_s));
			if (index > best_index) {
				best_index = index;
				best = j;
			}
		}
		done[best] = true;
		order.push_back(best);
		clock += inst.SetupCost(previous, best + 1) + inst.processing[best];
		previous = best + 1;
	}
	return Evaluate(inst, std::move(order));
}

//! ATCS, then a real neighbourhood: adjacent swaps, ARBITRARY-PAIR swaps
//! and RELOCATION of a single job to any other position.
//!
//! Adjacent swaps alone were far too weak. On the P6 pilot instance
//! (10 jobs, asymmetric setups) that version returned 168 against a
//! Held-Karp optimum of 60 — worse than the plain SQL an LLM writes for
//! the same problem, which is the opposite of what a vocabulary is for.
//! Relocation is what actually matters under sequence-dependent setups:
//! moving one job next to a cheap predecessor can save more than any
//! number of neighbour exchanges.
Schedule TardinessLocalSearch(const Instance &inst) {
	Schedule best = ApparentTardinessCost(inst);
	const idx_t n = best.order.size();
	bool improved = true;
	while (improved) {
		improved = false;
		// Swap any pair, not just neighbours.
		for (idx_t i = 0; i < n && !improved; i++) {
			for (idx_t j = i + 1; j < n; j++) {
				auto candidate = best.order;
				std::swap(candidate[i], candidate[j]);
				auto trial = Evaluate(inst, std::move(candidate));
				if (trial.total_weighted_tardiness < best.total_weighted_tardiness - 1e-12) {
					best = std::move(trial);
					improved = true;
					break;
				}
			}
		}
		// Relocate one job to every other position.
		for (idx_t i = 0; i < n && !improved; i++) {
			for (idx_t j = 0; j < n; j++) {
				if (i == j) {
					continue;
				}
				auto candidate = best.order;
				const auto job = candidate[i];
				candidate.erase(candidate.begin() + static_cast<int64_t>(i));
				candidate.insert(candidate.begin() + static_cast<int64_t>(j), job);
				auto trial = Evaluate(inst, std::move(candidate));
				if (trial.total_weighted_tardiness < best.total_weighted_tardiness - 1e-12) {
					best = std::move(trial);
					improved = true;
					break;
				}
			}
		}
	}
	return best;
}

//! The PROVEN optimum, by dynamic programming over (scheduled set, last
//! job) keeping a PARETO FRONTIER of (elapsed time, cost) per state.
//!
//! The frontier is the whole point. A plain Held-Karp that stores only the
//! cheapest way to reach (set, last) is WRONG here: with
//! sequence-dependent setups the elapsed clock depends on the path taken,
//! not just on which jobs are done, so a costlier-but-earlier prefix can
//! beat a cheaper-but-later one on everything that follows. The first
//! version of this function did exactly that and returned 91 on the P6
//! pilot instance while the local search returned 60 — an "exact" solver
//! losing to a heuristic, which is how the bug was caught.
//!
//! A pair (t1,c1) dominates (t2,c2) when t1 <= t2 and c1 <= c2: it is
//! never later and never dearer, so nothing reachable from the second is
//! better than from the first.
static constexpr idx_t SCHEDULE_EXACT_MAX_JOBS = 14;

Schedule ExactSchedule(const Instance &inst) {
	const idx_t n = inst.n;
	if (n > SCHEDULE_EXACT_MAX_JOBS) {
		throw InvalidInputException(
		    "anofox_optimize_schedule_exact is exponential and refuses more than %llu jobs "
		    "(got %llu). Use anofox_optimize_schedule_local_search or "
		    "anofox_optimize_schedule_best_of, which handle any size but are heuristic.",
		    static_cast<unsigned long long>(SCHEDULE_EXACT_MAX_JOBS),
		    static_cast<unsigned long long>(n));
	}
	if (n == 0) {
		return Evaluate(inst, {});
	}
	struct Label {
		double time;
		double cost;
		idx_t prev_last; // job index of the predecessor, n for "none"
		idx_t prev_idx;  // index into the predecessor state's label list
	};
	const idx_t full = idx_t(1) << n;
	vector<vector<vector<Label>>> lab(full, vector<vector<Label>>(n));

	for (idx_t j = 0; j < n; j++) {
		const double t = inst.SetupCost(0, j + 1) + inst.processing[j];
		lab[idx_t(1) << j][j].push_back(
		    Label {t, inst.priority[j] * std::max(0.0, t - inst.due[j]), n, 0});
	}
	// Insert a candidate, discarding it if dominated and evicting anything
	// it dominates. Keeps each frontier small in practice.
	auto offer = [](vector<Label> &frontier, const Label &cand) {
		for (const auto &l : frontier) {
			if (l.time <= cand.time + 1e-12 && l.cost <= cand.cost + 1e-12) {
				return;
			}
		}
		frontier.erase(std::remove_if(frontier.begin(), frontier.end(),
		                              [&](const Label &l) {
			                              return cand.time <= l.time + 1e-12 &&
			                                     cand.cost <= l.cost + 1e-12;
		                              }),
		               frontier.end());
		frontier.push_back(cand);
	};

	for (idx_t mask = 1; mask < full; mask++) {
		for (idx_t last = 0; last < n; last++) {
			if (!(mask >> last & 1) || lab[mask][last].empty()) {
				continue;
			}
			for (idx_t li = 0; li < lab[mask][last].size(); li++) {
				const Label base = lab[mask][last][li];
				for (idx_t nxt = 0; nxt < n; nxt++) {
					if (mask >> nxt & 1) {
						continue;
					}
					const double t =
					    base.time + inst.SetupCost(last + 1, nxt + 1) + inst.processing[nxt];
					const double c =
					    base.cost + inst.priority[nxt] * std::max(0.0, t - inst.due[nxt]);
					offer(lab[mask | (idx_t(1) << nxt)][nxt], Label {t, c, last, li});
				}
			}
		}
	}

	idx_t best_last = 0, best_idx = 0;
	double best_cost = std::numeric_limits<double>::infinity();
	for (idx_t j = 0; j < n; j++) {
		for (idx_t i = 0; i < lab[full - 1][j].size(); i++) {
			if (lab[full - 1][j][i].cost < best_cost) {
				best_cost = lab[full - 1][j][i].cost;
				best_last = j;
				best_idx = i;
			}
		}
	}
	vector<idx_t> order;
	idx_t mask = full - 1, cur = best_last, idx = best_idx;
	while (cur != n) {
		order.push_back(cur);
		const Label &l = lab[mask][cur][idx];
		const idx_t pl = l.prev_last, pi = l.prev_idx;
		mask ^= (idx_t(1) << cur);
		cur = pl;
		idx = pi;
	}
	std::reverse(order.begin(), order.end());
	return Evaluate(inst, std::move(order));
}

Schedule BestOfSchedule(const Instance &inst) {
	auto best = TardinessLocalSearch(inst);
	// Deliberately NOT calling ExactSchedule here: best_of must stay usable
	// at any size, and silently going exponential past some hidden
	// threshold is exactly the kind of surprise this library should not
	// ship. A caller who wants the optimum asks for it by name.
	for (auto candidate : {ApparentTardinessCost(inst), EarliestDueDate(inst),
	                       WeightedShortestProcessing(inst)}) {
		if (candidate.total_weighted_tardiness < best.total_weighted_tardiness) {
			best = candidate;
		}
	}
	return best;
}

vector<double> ReadList(Vector &v, idx_t count, idx_t row, const char *what, idx_t expect) {
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
			throw InvalidInputException("%s entry %llu is NULL — a NULL cannot be compared, "
			                            "so that job would be silently unschedulable",
			                            what, static_cast<uint64_t>(i));
		}
		const double x = values[ci];
		if (!std::isfinite(x)) {
			throw InvalidInputException("%s entry %llu is %f — values must be finite; a "
			                            "non-finite value silently defeats every comparison",
			                            what, static_cast<uint64_t>(i), x);
		}
		out.push_back(x);
	}
	return out;
}

} // namespace

static void AddSchedulingFunction(ExtensionLoader &loader, const string &name,
                                  Schedule (*fn)(const Instance &), const string &description,
                                  const string &example, const string &alias_of) {
	auto return_type =
	    LogicalType::STRUCT({{"order", LogicalType::LIST(LogicalType::UBIGINT)},
	                         {"total_weighted_tardiness", LogicalType::DOUBLE},
	                         {"makespan", LogicalType::DOUBLE}});
	ScalarFunction function(
	    name,
	    {LogicalType::LIST(LogicalType::DOUBLE), LogicalType::LIST(LogicalType::DOUBLE),
	     LogicalType::LIST(LogicalType::DOUBLE), LogicalType::LIST(LogicalType::DOUBLE),
	     LogicalType::BIGINT},
	    return_type, [fn, name](DataChunk &args, ExpressionState &, Vector &result) {
#ifdef ANOFOX_TELEMETRY_ENABLED
		    PostHogTelemetry::Instance().RecordFunctionCall(name);
#endif
		    const idx_t count = args.size();
		    result.SetVectorType(VectorType::FLAT_VECTOR);
		    UnifiedVectorFormat n_data;
		    args.data[4].ToUnifiedFormat(count, n_data);
		    auto n_vals = UnifiedVectorFormat::GetData<int64_t>(n_data);

		    auto &entries = StructVector::GetEntries(result);
		    auto &order_vec = *entries[0];
		    auto order_out = FlatVector::GetData<list_entry_t>(order_vec);
		    auto twt_out = FlatVector::GetData<double>(*entries[1]);
		    auto mk_out = FlatVector::GetData<double>(*entries[2]);
		    idx_t offset = 0;

		    for (idx_t row = 0; row < count; row++) {
			    const auto ni = n_data.sel->get_index(row);
			    if (!n_data.validity.RowIsValid(ni)) {
				    FlatVector::SetNull(result, row, true);
				    order_out[row].offset = offset;
				    order_out[row].length = 0;
				    twt_out[row] = 0;
				    mk_out[row] = 0;
				    continue;
			    }
			    const int64_t n_signed = n_vals[ni];
			    if (n_signed < 0) {
				    throw InvalidInputException("job count must be >= 0, got %lld",
				                                static_cast<long long>(n_signed));
			    }
			    Instance inst;
			    inst.n = static_cast<idx_t>(n_signed);
			    inst.processing = ReadList(args.data[0], count, row, "processing_times", inst.n);
			    inst.due = ReadList(args.data[1], count, row, "due_dates", inst.n);
			    inst.priority = ReadList(args.data[2], count, row, "priorities", inst.n);
			    inst.setup =
			        ReadList(args.data[3], count, row, "setup matrix", (inst.n + 1) * (inst.n + 1));
			    for (idx_t i = 0; i < inst.n; i++) {
				    if (inst.processing[i] < 0) {
					    throw InvalidInputException(
					        "job %llu has processing time %f — must be >= 0; a negative "
					        "duration would let the clock run backwards",
					        static_cast<uint64_t>(i), inst.processing[i]);
				    }
			    }

			    const auto s = inst.n == 0 ? Schedule {} : fn(inst);
			    twt_out[row] = s.total_weighted_tardiness;
			    mk_out[row] = s.makespan;
			    order_out[row].offset = offset;
			    order_out[row].length = s.order.size();
			    ListVector::Reserve(order_vec, offset + s.order.size());
			    auto child = FlatVector::GetData<uint64_t>(ListVector::GetEntry(order_vec));
			    for (idx_t i = 0; i < s.order.size(); i++) {
				    child[offset + i] = s.order[i];
			    }
			    offset += s.order.size();
		    }
		    FlatVector::Validity(ListVector::GetEntry(order_vec)).SetAllValid(offset);
		    ListVector::SetListSize(order_vec, offset);
	    });

	RegisterScalarOrAlias(loader, std::move(function), description, example, alias_of);
}

static void AddSchedulingFamily(ExtensionLoader &loader, const string &short_name,
                                Schedule (*fn)(const Instance &), const string &description,
                                const string &example) {
	const string canonical = "anofox_optimize_" + short_name;
	AddSchedulingFunction(loader, canonical, fn, description, "anofox_optimize_" + example, "");
	AddSchedulingFunction(loader, "opt_" + short_name, fn, description, "opt_" + example, canonical);
}

void RegisterSchedulingFunctions(ExtensionLoader &loader) {
	const string shape =
	    " Takes processing_times, due_dates and priorities (one per job) plus the "
	    "setup matrix flattened ROW-MAJOR over n+1 rows, where index 0 is the VIRTUAL "
	    "START: setup[i*(n+1)+j] is the cost of running job j-1 after job i-1, and row "
	    "0 is the cost of running a job first. Returns a 0-based job order, the total "
	    "priority-weighted tardiness of that order, and its makespan.";
	const string ex = "([10.0,10.0],[20.0,5.0],[1.0,3.0],[5.0,5.0,5.0,0.0,2.0,2.0,0.0,2.0,2.0], 2)";

	AddSchedulingFamily(loader, "schedule_edd", EarliestDueDate,
	                    "Sequences jobs by EARLIEST DUE DATE. The textbook rule and optimal "
	                    "for maximum lateness without setups, but it ignores priorities and "
	                    "setup costs entirely, so it can be poor on weighted tardiness." + shape,
	                    "schedule_edd" + ex);
	AddSchedulingFamily(loader, "schedule_wspt", WeightedShortestProcessing,
	                    "Sequences jobs by WEIGHTED SHORTEST PROCESSING TIME "
	                    "(priority/processing, descending). Strong when nearly everything "
	                    "will be late, weak when only a few jobs are, because it ignores due "
	                    "dates and setups." + shape,
	                    "schedule_wspt" + ex);
	AddSchedulingFamily(loader, "schedule_atcs", ApparentTardinessCost,
	                    "Sequences jobs by APPARENT TARDINESS COST WITH SETUPS: at each step "
	                    "picks the job maximising priority/processing, discounted by its "
	                    "slack and by the setup needed to switch to it. Unlike EDD and WSPT "
	                    "it weighs due dates, priorities AND setups together, which is what "
	                    "this objective actually trades off." + shape,
	                    "schedule_atcs" + ex);
	AddSchedulingFamily(loader, "schedule_exact", ExactSchedule,
	                    "The PROVEN OPTIMUM by Held-Karp dynamic programming over (scheduled "
	                    "set, last job). Weighted tardiness with sequence-dependent setups is "
	                    "NP-hard, so this is exponential: it REFUSES more than 16 jobs with an "
	                    "error naming the heuristic to use instead, rather than running for "
	                    "hours. Use it to check how far a heuristic really is on a small "
	                    "instance." + shape,
	                    "schedule_exact" + ex);
	AddSchedulingFamily(loader, "schedule_local_search", TardinessLocalSearch,
	                    "Sequences by ATCS and then improves with adjacent-pair swaps while "
	                    "they reduce weighted tardiness. Slower than the dispatch rules and "
	                    "usually better than any of them alone." + shape,
	                    "schedule_local_search" + ex);
	AddSchedulingFamily(loader, "schedule_best_of", BestOfSchedule,
	                    "Runs every scheduling algorithm in this family and returns whichever "
	                    "had the lowest total weighted tardiness. Costs the sum of the parts "
	                    "and never loses to any single member." + shape,
	                    "schedule_best_of" + ex);
}

} // namespace duckdb
