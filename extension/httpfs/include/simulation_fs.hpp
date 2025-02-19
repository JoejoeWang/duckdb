#pragma once

#include "httpfs.hpp"

namespace duckdb {

/**
 * SimulationFileSystem is a mock implementation that inherits from HTTPFileSystem.
 * It simulates remote requests by inserting artificial latency, while storing data locally.
 */
class SimulationFileSystem : public HTTPFileSystem {
public:
	/**
	 * Determines if this file system can handle the given path. Recognizes 'sim://' prefix.
	 */
	bool CanHandleFile(const std::string &fpath) override;

	/**
	 * Issues a simulated range request. Inserts latency, maps 'sim://' path to local disk, and performs a local read.
	 */
	duckdb::unique_ptr<ResponseWrapper> GetRangeRequest(FileHandle &handle, std::string url, HeaderMap header_map,
	                                                    idx_t file_offset, char *buffer_out,
	                                                    idx_t buffer_out_len) override;

	/**
	 * Issues a simulated PUT request. Inserts latency, maps 'sim://' path to local disk, and writes the data locally.
	 */
	duckdb::unique_ptr<ResponseWrapper> PutRequest(FileHandle &handle, std::string url, HeaderMap header_map,
	                                               char *buffer_in, idx_t buffer_in_len,
	                                               std::string http_params = "") override;

	/**
	 * Issues a simulated HEAD request. Inserts latency, maps 'sim://' path to local disk, and retrieves file metadata.
	 */
	duckdb::unique_ptr<ResponseWrapper> HeadRequest(FileHandle &handle, std::string url, HeaderMap header_map) override;

private:
	/**
	 * Simulates network round-trip latency by sleeping for a fixed duration (e.g., 100ms).
	 */
	void SimulateLatency();

	/**
	 * Converts a 'sim://' URL to a local path for storing or reading data.
	 */
	std::string MapSimURLToLocal(const std::string &url);

	/**
	 * Performs a local file range read. It seeks to file_offset and reads buffer_out_len bytes into buffer_out.
	 */
	duckdb::unique_ptr<ResponseWrapper> LocalRangeRead(const std::string &local_path, idx_t file_offset,
	                                                   char *buffer_out, idx_t buffer_out_len);

	/**
	 * Performs a local file write by overwriting or creating a new file with the given data.
	 */
	duckdb::unique_ptr<ResponseWrapper> LocalWrite(const std::string &local_path, char *buffer_in, idx_t buffer_in_len);
};

} // namespace duckdb
