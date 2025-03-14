#include "simulation_fs.hpp"

#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/database.hpp"

#include <chrono>
#include <fstream>
#include <thread>

namespace duckdb {

// A global fallback cache if the user doesn't have a context-based one
SimulationMetadataCache SimulationFileSystem::global_fallback_cache(false, true);

/**
 * SimulationFileHandle constructor
 */
SimulationFileHandle::SimulationFileHandle(FileSystem &fs, const std::string &path, FileOpenFlags flags_p)
    : HTTPFileHandle(fs, path, flags_p, HTTPParams()), cached_file_size(0), file_fully_cached(false),
      stored_opener(nullptr) {
}

/**
 * Called after creation inside CreateHandle(...). We store 'file_opener' in
 * 'stored_opener' so we can retrieve it from HeadRequest, PutRequest, etc.
 */
void SimulationFileHandle::Initialize(optional_ptr<FileOpener> file_opener) {
	HTTPFileHandle::Initialize(file_opener);
	this->stored_opener = file_opener;

	auto &sim_fs = file_system.Cast<SimulationFileSystem>();

	// do a HEAD request to find length
	HeaderMap dummy;
	auto head_response = sim_fs.HeadRequest(*this, path, dummy);
	if (head_response->code == 200) {
		auto it = head_response->headers.find("Content-Length");
		if (it != head_response->headers.end()) {
			try {
				length = std::stoll(it->second);
			} catch (...) {
				length = 0;
			}
		}
	}

	// e.g. if <5MB => read entire file
	if (length > 0 && length < 5 * 1024 * 1024) {
		file_fully_cached = true;
		cached_file_size = length;
		cached_file_data = duckdb::unique_ptr<char[]>(new char[length]);

		sim_fs.SimulateLatency();
		std::string local_path = sim_fs.MapSimURLToLocal(path);
		std::ifstream file(local_path, std::ios::binary);
		if (!file.good()) {
			file_fully_cached = false;
			return;
		}
		file.read(cached_file_data.get(), length);
		file.close();
	}
}

/**
 * Check if path starts with 'sim://'
 */
bool SimulationFileSystem::CanHandleFile(const std::string &fpath) {
	return StringUtil::StartsWith(fpath, "sim://");
}

/**
 * Instead of overriding OpenFile(...) (which is final), we override CreateHandle(...)
 * to produce a SimulationFileHandle. This is the standard approach in HTTPFileSystem.
 */
duckdb::unique_ptr<HTTPFileHandle> SimulationFileSystem::CreateHandle(const std::string &path, FileOpenFlags flags,
                                                                      optional_ptr<FileOpener> opener) {
	auto result = make_uniq<SimulationFileHandle>(*this, path, flags);
	result->Initialize(opener);
	return std::move(result);
}

/**
 * HEAD request: read or populate SimulationMetadataCache
 */
duckdb::unique_ptr<ResponseWrapper> SimulationFileSystem::HeadRequest(FileHandle &handle, std::string url,
                                                                      HeaderMap header_map) {
	SimulateLatency();

	auto response = make_uniq<ResponseWrapper>();
	response->http_url = url;

	// We want to retrieve the opener from the handle
	auto &sim_handle = handle.Cast<SimulationFileHandle>();
	auto opener = sim_handle.stored_opener;

	auto cache_ptr = GetSimulationCache(opener);
	if (!cache_ptr) {
		cache_ptr = &global_fallback_cache;
	}

	SimulationCacheEntry entry;
	if (cache_ptr->Find(url, entry)) {
		response->code = 200;
		response->headers["Content-Length"] = std::to_string(entry.length);
		return response;
	}

	// If not found in cache => do local read
	std::string local_path = MapSimURLToLocal(url);
	std::ifstream file(local_path, std::ios::binary);
	if (!file.good()) {
		response->code = 404;
		response->error = "File not found in HEAD: " + local_path;
		return response;
	}
	file.seekg(0, std::ios::end);
	idx_t sz = idx_t(file.tellg());
	file.close();

	response->code = 200;
	response->headers["Content-Length"] = std::to_string(sz);

	SimulationCacheEntry new_entry;
	new_entry.length = sz;
	new_entry.last_modified = 0;
	cache_ptr->Insert(url, new_entry);

	return response;
}

/**
 * If fully cached, do a memcpy. Else local partial read.
 */
duckdb::unique_ptr<ResponseWrapper> SimulationFileSystem::GetRangeRequest(FileHandle &handle, std::string url,
                                                                          HeaderMap header_map, idx_t file_offset,
                                                                          char *buffer_out, idx_t buffer_out_len) {
	SimulateLatency();

	auto &sim_handle = handle.Cast<SimulationFileHandle>();
	if (sim_handle.file_fully_cached && sim_handle.cached_file_data) {
		idx_t max_read = (sim_handle.cached_file_size > file_offset) ? (sim_handle.cached_file_size - file_offset) : 0;
		idx_t read_len = (buffer_out_len > max_read) ? max_read : buffer_out_len;
		auto r = make_uniq<ResponseWrapper>();
		r->http_url = url;

		if (read_len == 0) {
			r->code = 416;
			r->error = "Offset beyond file size in memory cache";
			return r;
		}
		memcpy(buffer_out, sim_handle.cached_file_data.get() + file_offset, read_len);

		r->code = 206;
		r->headers["Content-Range"] = "bytes " + std::to_string(file_offset) + "-" +
		                              std::to_string(file_offset + read_len - 1) + "/" +
		                              std::to_string(sim_handle.cached_file_size);
		return r;
	}

	// otherwise local partial read
	std::string local_path = MapSimURLToLocal(url);
	return LocalRangeRead(local_path, file_offset, buffer_out, buffer_out_len);
}

/**
 * PutRequest => do local write, remove from cache if any
 */
duckdb::unique_ptr<ResponseWrapper> SimulationFileSystem::PutRequest(FileHandle &handle, std::string url,
                                                                     HeaderMap header_map, char *buffer_in,
                                                                     idx_t buffer_in_len, std::string http_params) {
	SimulateLatency();

	auto &sim_handle = handle.Cast<SimulationFileHandle>();
	sim_handle.file_fully_cached = false;
	sim_handle.cached_file_size = 0;
	sim_handle.cached_file_data.reset();

	// remove from metadata cache
	auto opener = sim_handle.stored_opener;
	auto cache_ptr = GetSimulationCache(opener);
	if (!cache_ptr) {
		cache_ptr = &global_fallback_cache;
	}
	cache_ptr->Erase(url);

	// now do local write
	std::string local_path = MapSimURLToLocal(url);
	return LocalWrite(local_path, buffer_in, buffer_in_len);
}

void SimulationFileSystem::SimulateLatency() {
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

std::string SimulationFileSystem::MapSimURLToLocal(const std::string &url) {
	static const std::string prefix = "sim://";
	auto suffix = url.substr(prefix.size());
	return "/tmp/simfs/" + suffix;
}

duckdb::unique_ptr<ResponseWrapper> SimulationFileSystem::LocalRangeRead(const std::string &local_path,
                                                                         idx_t file_offset, char *buffer_out,
                                                                         idx_t buffer_out_len) {
	auto resp = make_uniq<ResponseWrapper>();
	resp->http_url = local_path;

	std::ifstream file(local_path, std::ios::binary);
	if (!file.good()) {
		resp->code = 404;
		resp->error = "File not found: " + local_path;
		return resp;
	}
	file.seekg(0, std::ios::end);
	idx_t file_size = idx_t(file.tellg());
	if (file_offset >= file_size) {
		resp->code = 416;
		resp->error = "Offset beyond file size";
		return resp;
	}
	idx_t read_size = (file_offset + buffer_out_len > file_size) ? (file_size - file_offset) : buffer_out_len;
	file.seekg(file_offset);
	file.read(buffer_out, read_size);

	resp->code = 206;
	resp->headers["Content-Range"] = "bytes " + std::to_string(file_offset) + "-" +
	                                 std::to_string(file_offset + read_size - 1) + "/" + std::to_string(file_size);
	return resp;
}

duckdb::unique_ptr<ResponseWrapper> SimulationFileSystem::LocalWrite(const std::string &local_path, char *buffer_in,
                                                                     idx_t buffer_in_len) {
	auto resp = make_uniq<ResponseWrapper>();
	resp->http_url = local_path;

	std::ofstream outfile(local_path, std::ios::binary | std::ios::trunc);
	if (!outfile.good()) {
		resp->code = 403;
		resp->error = "Could not open file for write: " + local_path;
		return resp;
	}
	outfile.write(buffer_in, buffer_in_len);
	outfile.close();

	resp->code = 200;
	return resp;
}

/**
 * If we have a client context, we get a registered 'SimulationMetadataCache'.
 * Else we fallback to a global one.
 */
optional_ptr<SimulationMetadataCache> SimulationFileSystem::GetSimulationCache(optional_ptr<FileOpener> opener) {
	if (!opener) {
		return &global_fallback_cache;
	}
	auto client_context = FileOpener::TryGetClientContext(opener);
	if (!client_context) {
		return &global_fallback_cache;
	}
	auto &state_map = client_context->registered_state;
	auto cache_ptr = state_map->GetOrCreate<SimulationMetadataCache>("sim_cache");
	if (!cache_ptr) {
		// fallback
		return &global_fallback_cache;
	}
	return (SimulationMetadataCache *)cache_ptr.get();
}

} // namespace duckdb
