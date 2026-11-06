#pragma once
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <vector>

namespace duckdb {

// The fit rule every capacity-bounded family shares: a bin, wave or knapsack
// selection fits when its sizes, summed in ITEM order (input list order) in
// doubles, are at most the capacity, with no tolerance. That is the number
// list_sum(list(size ORDER BY item)) returns, so a SQL checker can repeat it.
// A running load summed in placement order is not enough: doubles do not add
// associatively, and packed largest first 0.3 + 0.2 + 0.1 is 0.6 while in
// item order 0.1 + 0.2 + 0.3 is 0.6000000000000001.

//! The sizes of `members` (ascending item indices) summed in item order, with
//! item `extra` added in its place when it is not INVALID_INDEX.
inline double ItemOrderLoad(const vector<double> &sizes, const vector<idx_t> &members,
                            idx_t extra = DConstants::INVALID_INDEX) {
	double load = 0;
	bool added = extra == DConstants::INVALID_INDEX;
	for (auto i : members) {
		if (!added && extra < i) {
			load += sizes[extra];
			added = true;
		}
		load += sizes[i];
	}
	if (!added) {
		load += sizes[extra];
	}
	return load;
}

//! Make every group fit by that rule. `group` holds a 0-based group per
//! item, using groups 0..count-1. While a group is over, its first item by
//! `move_first` (among those of positive size) moves to the first other group
//! it fits in item order, else to a new group at the end. A group only ever
//! receives an item it fits with, and an over group always keeps an item
//! (every size is at most the capacity, so one alone fits), so no group
//! empties and the numbering stays contiguous. Returns the group count.
template <class MoveFirst>
idx_t SettleInItemOrder(const vector<double> &sizes, double capacity, vector<idx_t> &group, idx_t count,
                        MoveFirst move_first) {
	vector<vector<idx_t>> members(count);
	vector<double> loads(count, 0.0);
	for (idx_t i = 0; i < sizes.size(); i++) {
		// The precondition that makes this end: an item over the capacity
		// would move to a new group forever. Callers reject such items.
		if (sizes[i] > capacity) {
			throw InternalException("SettleInItemOrder: item %llu of size %f exceeds capacity %f",
			                        static_cast<uint64_t>(i), sizes[i], capacity);
		}
		members[group[i]].push_back(i);
		loads[group[i]] += sizes[i];
	}
	for (idx_t g = 0; g < members.size(); g++) {
		while (loads[g] > capacity) {
			// The load is over a capacity > 0, so some member has positive size.
			idx_t item = DConstants::INVALID_INDEX;
			for (auto i : members[g]) {
				if (sizes[i] > 0 && (item == DConstants::INVALID_INDEX || move_first(i, item))) {
					item = i;
				}
			}
			members[g].erase(std::find(members[g].begin(), members[g].end(), item));
			loads[g] = ItemOrderLoad(sizes, members[g]);
			idx_t to = members.size();
			for (idx_t h = 0; h < members.size(); h++) {
				if (h != g && ItemOrderLoad(sizes, members[h], item) <= capacity) {
					to = h;
					break;
				}
			}
			if (to == members.size()) {
				members.emplace_back();
				loads.push_back(0);
			}
			auto &into = members[to];
			into.insert(std::lower_bound(into.begin(), into.end(), item), item);
			loads[to] = ItemOrderLoad(sizes, into);
			group[item] = to;
		}
	}
	return members.size();
}

} // namespace duckdb
