#include "simulation_fs.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

#include <chrono>
#include <fstream>
#include <thread>

namespace duckdb {

/**
 * Determines if this file system can handle the given path. Recognizes 'sim://' prefix.
 */
bool SimulationFileSystem::CanHandleFile(const std::string &fpath) {
	return StringUtil::StartsWith(fpath, "sim://");
}

/**
 * Issues a simulated range request. Inserts latency, maps 'sim://' path to local disk, and performs a local read.
 */
duckdb::unique_ptr<ResponseWrapper> SimulationFileSystem::GetRangeRequest(FileHandle &handle, std::string url,
                                                                          HeaderMap header_map, idx_t file_offset,
                                                                          char *buffer_out, idx_t buffer_out_len) {
	SimulateLatency();
	std::string local_path = MapSimURLToLocal(url);
	return LocalRangeRead(local_path, file_offset, buffer_out, buffer_out_len);
}

/**
 * Issues a simulated PUT request. Inserts latency, maps 'sim://' path to local disk, and writes the data locally.
 */
duckdb::unique_ptr<ResponseWrapper> SimulationFileSystem::PutRequest(FileHandle &handle, std::string url,
                                                                     HeaderMap header_map, char *buffer_in,
                                                                     idx_t buffer_in_len, std::string http_params) {
	SimulateLatency();
	std::string local_path = MapSimURLToLocal(url);
	return LocalWrite(local_path, buffer_in, buffer_in_len);
}

/**
 * Issues a simulated HEAD request. Inserts latency, maps 'sim://' path to local disk, and retrieves file metadata.
 */
duckdb::unique_ptr<ResponseWrapper> SimulationFileSystem::HeadRequest(FileHandle &handle, std::string url,
                                                                      HeaderMap header_map) {
	SimulateLatency();
	auto response = duckdb::make_uniq<ResponseWrapper>();
	std::string local_path = MapSimURLToLocal(url);

	std::ifstream file(local_path, std::ios::binary);
	if (!file.good()) {
		response->code = 404;
		response->error = "File not found: " + local_path;
		response->http_url = url;
		return response;
	}
	file.seekg(0, std::ios::end);
	std::streamoff size = file.tellg();
	file.close();

	response->code = 200;
	response->headers["Content-Length"] = std::to_string(size);
	response->http_url = url;
	return response;
}

/**
 * Simulates network round-trip latency by sleeping for a fixed duration (e.g., 100ms).
 */
void SimulationFileSystem::SimulateLatency() {
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

/**
 * Converts a 'sim://' URL to a local path for storing or reading data.
 */
std::string SimulationFileSystem::MapSimURLToLocal(const std::string &url) {
	static const std::string prefix = "sim://";
	std::string suffix = url.substr(prefix.size());
	return "/tmp/simfs/" + suffix;
}

/**
 * Performs a local file range read. It seeks to file_offset and reads buffer_out_len bytes into buffer_out.
 */
duckdb::unique_ptr<ResponseWrapper> SimulationFileSystem::LocalRangeRead(const std::string &local_path,
                                                                         idx_t file_offset, char *buffer_out,
                                                                         idx_t buffer_out_len) {
	auto response = duckdb::make_uniq<ResponseWrapper>();
	response->http_url = local_path;

	std::ifstream file(local_path, std::ios::binary);
	if (!file.good()) {
		response->code = 404;
		response->error = "File not found: " + local_path;
		return response;
	}

	file.seekg(0, std::ios::end);
	idx_t file_size = idx_t(file.tellg());
	if (file_offset >= file_size) {
		response->code = 416;
		response->error = "Offset beyond file size";
		return response;
	}

	idx_t read_size = (file_offset + buffer_out_len > file_size) ? (file_size - file_offset) : buffer_out_len;
	file.seekg(file_offset);
	file.read(buffer_out, read_size);

	response->code = 206;
	response->headers["Content-Range"] = "bytes " + std::to_string(file_offset) + "-" +
	                                     std::to_string(file_offset + read_size - 1) + "/" + std::to_string(file_size);
	return response;
}

/**
 * Performs a local file write by overwriting or creating a new file with the given data.
 */
duckdb::unique_ptr<ResponseWrapper> SimulationFileSystem::LocalWrite(const std::string &local_path, char *buffer_in,
                                                                     idx_t buffer_in_len) {
	auto response = duckdb::make_uniq<ResponseWrapper>();
	response->http_url = local_path;

	std::ofstream file(local_path, std::ios::binary | std::ios::trunc);
	if (!file.good()) {
		response->code = 403;
		response->error = "Could not open file for write: " + local_path;
		return response;
	}
	file.write(buffer_in, buffer_in_len);
	file.close();

	response->code = 200;
	return response;
}

} // namespace duckdb
