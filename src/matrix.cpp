#include "matrix.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

#include "register_alias.hpp"

#ifdef ANOFOX_TELEMETRY_ENABLED
#include "telemetry.hpp"
#endif

#include <cmath>
#include <vector>

namespace duckdb {

namespace {

struct ListReader {
	UnifiedVectorFormat outer, child;
	const list_entry_t *lists;
	Vector *vec;
};

ListReader OpenList(Vector &v, idx_t count) {
	ListReader r;
	r.vec = &v;
	v.ToUnifiedFormat(count, r.outer);
	r.lists = UnifiedVectorFormat::GetData<list_entry_t>(r.outer);
	ListVector::GetEntry(v).ToUnifiedFormat(ListVector::GetListSize(v), r.child);
	return r;
}

} // namespace

void RegisterMatrixFunctions(ExtensionLoader &loader) {
	auto build = [](DataChunk &args, ExpressionState &, Vector &result) {
#ifdef ANOFOX_TELEMETRY_ENABLED
		PostHogTelemetry::Instance().RecordFunctionCall("anofox_optimize_matrix_from_triples");
#endif
		const idx_t count = args.size();
		result.SetVectorType(VectorType::FLAT_VECTOR);

		auto from_r = OpenList(args.data[0], count);
		auto to_r = OpenList(args.data[1], count);
		auto val_r = OpenList(args.data[2], count);
		auto from_vals = UnifiedVectorFormat::GetData<int64_t>(from_r.child);
		auto to_vals = UnifiedVectorFormat::GetData<int64_t>(to_r.child);
		auto value_vals = UnifiedVectorFormat::GetData<double>(val_r.child);

		UnifiedVectorFormat n_data, base_data;
		args.data[3].ToUnifiedFormat(count, n_data);
		args.data[4].ToUnifiedFormat(count, base_data);
		auto ns = UnifiedVectorFormat::GetData<int64_t>(n_data);
		auto bases = UnifiedVectorFormat::GetData<int64_t>(base_data);

		auto out = FlatVector::GetData<list_entry_t>(result);
		idx_t offset = 0;

		for (idx_t row = 0; row < count; row++) {
			const auto ni = n_data.sel->get_index(row);
			const auto bi = base_data.sel->get_index(row);
			if (!n_data.validity.RowIsValid(ni) || !base_data.validity.RowIsValid(bi)) {
				FlatVector::SetNull(result, row, true);
				out[row].offset = offset;
				out[row].length = 0;
				continue;
			}
			if (ns[ni] < 0) {
				throw InvalidInputException("n must be >= 0, got %lld",
				                            static_cast<long long>(ns[ni]));
			}
			const idx_t n = static_cast<idx_t>(ns[ni]);
			const int64_t base = bases[bi];

			const auto fe = from_r.lists[from_r.outer.sel->get_index(row)];
			const auto te = to_r.lists[to_r.outer.sel->get_index(row)];
			const auto ve = val_r.lists[val_r.outer.sel->get_index(row)];
			if (fe.length != te.length || fe.length != ve.length) {
				throw InvalidInputException(
				    "from, to and value must be the same length, got %llu, %llu and %llu — "
				    "each triple needs exactly one of each",
				    static_cast<uint64_t>(fe.length), static_cast<uint64_t>(te.length),
				    static_cast<uint64_t>(ve.length));
			}

			vector<double> m(n * n, 0.0);
			vector<bool> seen(n * n, false);
			for (idx_t i = 0; i < fe.length; i++) {
				const auto fi = from_r.child.sel->get_index(fe.offset + i);
				const auto ti = to_r.child.sel->get_index(te.offset + i);
				const auto vi = val_r.child.sel->get_index(ve.offset + i);
				if (!from_r.child.validity.RowIsValid(fi) ||
				    !to_r.child.validity.RowIsValid(ti) ||
				    !val_r.child.validity.RowIsValid(vi)) {
					throw InvalidInputException("triple %llu contains a NULL",
					                            static_cast<uint64_t>(i));
				}
				const int64_t f = from_vals[fi] - base;
				const int64_t t = to_vals[ti] - base;
				if (f < 0 || t < 0 || static_cast<idx_t>(f) >= n || static_cast<idx_t>(t) >= n) {
					throw InvalidInputException(
					    "triple %llu references (%lld, %lld) which is outside the %llux%llu "
					    "matrix once index_base %lld is subtracted — check n and index_base",
					    static_cast<uint64_t>(i), static_cast<long long>(from_vals[fi]),
					    static_cast<long long>(to_vals[ti]), static_cast<uint64_t>(n),
					    static_cast<uint64_t>(n), static_cast<long long>(base));
				}
				const double v = value_vals[vi];
				if (!std::isfinite(v)) {
					throw InvalidInputException("triple %llu has value %f — must be finite",
					                            static_cast<uint64_t>(i), v);
				}
				const idx_t cell = static_cast<idx_t>(f) * n + static_cast<idx_t>(t);
				if (seen[cell]) {
					throw InvalidInputException(
					    "triple %llu sets cell (%lld, %lld) which was already set — duplicate "
					    "entries would silently keep whichever came last",
					    static_cast<uint64_t>(i), static_cast<long long>(from_vals[fi]),
					    static_cast<long long>(to_vals[ti]));
				}
				seen[cell] = true;
				m[cell] = v;
			}

			out[row].offset = offset;
			out[row].length = m.size();
			ListVector::Reserve(result, offset + m.size());
			auto child = FlatVector::GetData<double>(ListVector::GetEntry(result));
			for (idx_t i = 0; i < m.size(); i++) {
				child[offset + i] = m[i];
			}
			offset += m.size();
		}
		FlatVector::Validity(ListVector::GetEntry(result)).SetAllValid(offset);
		ListVector::SetListSize(result, offset);
	};

	const string description =
	    "Builds the flattened ROW-MAJOR matrix the sequencing, scheduling, assortment "
	    "and portfolio functions expect, from (from, to, value) TRIPLES — which is how "
	    "the data is usually stored. Missing cells are 0; duplicate cells RAISE rather "
	    "than silently keeping the last one. `index_base` is subtracted from the ids, so "
	    "pass 1 for 1-based ids and 0 for 0-based. Use with list aggregation, e.g. "
	    "`SELECT anofox_optimize_matrix_from_triples(list(a), list(b), list(v), 8, 1) FROM t`.";

	const auto make = [&](const string &name) {
		return ScalarFunction(name,
		                      {LogicalType::LIST(LogicalType::BIGINT),
		                       LogicalType::LIST(LogicalType::BIGINT),
		                       LogicalType::LIST(LogicalType::DOUBLE), LogicalType::BIGINT,
		                       LogicalType::BIGINT},
		                      LogicalType::LIST(LogicalType::DOUBLE), build);
	};
	const string canonical = "anofox_optimize_matrix_from_triples";
	RegisterScalarOrAlias(loader, make(canonical), description,
	                      canonical + "([1,2], [2,1], [0.5, 0.5], 2, 1)", "");
	RegisterScalarOrAlias(loader, make("opt_matrix_from_triples"), description, "", canonical);
}

} // namespace duckdb
