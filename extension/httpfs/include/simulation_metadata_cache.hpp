#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/main/client_context_state.hpp"

#include <time.h>

namespace duckdb {

struct SimulationCacheEntry {
	idx_t length;
	time_t last_modified;
};

class SimulationMetadataCache : public ClientContextState {
public:
	explicit SimulationMetadataCache(bool flush_on_query_end_p, bool shared_p)
	    : flush_on_query_end(flush_on_query_end_p), shared(shared_p) {
	}

	void Insert(const std::string &path, const SimulationCacheEntry &val) {
		if (shared) {
			lock_guard<mutex> parallel_lock(lock);
			map[path] = val;
		} else {
			map[path] = val;
		}
	}

	void Erase(const std::string &path) {
		if (shared) {
			lock_guard<mutex> parallel_lock(lock);
			map.erase(path);
		} else {
			map.erase(path);
		}
	}

	bool Find(const std::string &path, SimulationCacheEntry &ret_val) {
		if (shared) {
			lock_guard<mutex> parallel_lock(lock);
			auto lookup = map.find(path);
			if (lookup != map.end()) {
				ret_val = lookup->second;
				return true;
			}
			return false;
		} else {
			auto lookup = map.find(path);
			if (lookup != map.end()) {
				ret_val = lookup->second;
				return true;
			}
			return false;
		}
	}

	void Clear() {
		if (shared) {
			lock_guard<mutex> parallel_lock(lock);
			map.clear();
		} else {
			map.clear();
		}
	}

	void QueryEnd(ClientContext &context) override {
		if (flush_on_query_end) {
			Clear();
		}
	}

private:
	mutex lock;
	unordered_map<std::string, SimulationCacheEntry> map;
	bool flush_on_query_end;
	bool shared;
};

} // namespace duckdb
