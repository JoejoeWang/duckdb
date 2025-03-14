#pragma once

#include "httpfs.hpp"
#include "simulation_metadata_cache.hpp"

namespace duckdb {

/**
 * SimulationFileHandle: extends HTTPFileHandle. We store a pointer to the FileOpener
 * so we can retrieve a SimulationMetadataCache or do other logic that normally
 * requires an opener.
 */
class SimulationFileHandle : public HTTPFileHandle {
public:
	SimulationFileHandle(FileSystem &fs, const std::string &path, FileOpenFlags flags);

	void Initialize(optional_ptr<FileOpener> file_opener) override;

	unique_ptr<char[]> cached_file_data;
	idx_t cached_file_size;
	bool file_fully_cached;

	// Store the opener here because FileHandle doesn't have an 'opener' member
	optional_ptr<FileOpener> stored_opener;
};

/**
 * SimulationFileSystem: a local-based FS that simulates remote overhead, uses
 * a simple metadata cache, and can store entire files in memory for small files.
 */
class SimulationFileSystem : public HTTPFileSystem {
public:
	bool CanHandleFile(const std::string &fpath) override;

	/**
	 * We do NOT override OpenFile(...) since it's final in HTTPFileSystem.
	 * Instead, we override CreateHandle(...) to produce a SimulationFileHandle.
	 */
	duckdb::unique_ptr<HTTPFileHandle> CreateHandle(const std::string &path, FileOpenFlags flags,
	                                                optional_ptr<FileOpener> opener) override;

	duckdb::unique_ptr<ResponseWrapper> HeadRequest(FileHandle &handle, std::string url, HeaderMap header_map) override;
	duckdb::unique_ptr<ResponseWrapper> GetRangeRequest(FileHandle &handle, std::string url, HeaderMap header_map,
	                                                    idx_t file_offset, char *buffer_out,
	                                                    idx_t buffer_out_len) override;
	duckdb::unique_ptr<ResponseWrapper> PutRequest(FileHandle &handle, std::string url, HeaderMap header_map,
	                                               char *buffer_in, idx_t buffer_in_len,
	                                               std::string http_params = "") override;

protected:
	duckdb::unique_ptr<ResponseWrapper> LocalRangeRead(const std::string &local_path, idx_t file_offset,
	                                                   char *buffer_out, idx_t buffer_out_len);
	duckdb::unique_ptr<ResponseWrapper> LocalWrite(const std::string &local_path, char *buffer_in, idx_t buffer_in_len);

	/**
	 * Returns or creates a SimulationMetadataCache for the given opener (ClientContext).
	 * If none found, fallback to a global cache.
	 */
	optional_ptr<SimulationMetadataCache> GetSimulationCache(optional_ptr<FileOpener> opener);

	// An optional static fallback if no context-based cache is found
	static SimulationMetadataCache global_fallback_cache;

public:
	void SimulateLatency();
	std::string MapSimURLToLocal(const std::string &url);
};

} // namespace duckdb
