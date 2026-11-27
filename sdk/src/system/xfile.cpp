/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/filesystem/vfs.h>
#include <rex/filesystem/devices/host_path_entry.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xfile.h>
#include <rex/thread/mutex.h>

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <span>
#include <string>
#include <vector>

namespace rex::system {

namespace {

bool tek6_env_enabled(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && (std::strcmp(value, "1") == 0 ||
                              std::strcmp(value, "true") == 0 ||
                              std::strcmp(value, "on") == 0);
}

bool tek6_td_source_offset(const std::string& logical_name, uint64_t offset,
                           uint32_t length, uint64_t* source_offset) {
  const char* spec = std::getenv("TEK6_RUNTIME_TD_REMAP");
  if (spec == nullptr || *spec == '\0') return false;

  const std::string expected_suffix = ".BIN";
  size_t begin = 0;
  const std::string remap_spec(spec);
  while (begin < remap_spec.size()) {
    const size_t comma = remap_spec.find(',', begin);
    const size_t end = comma == std::string::npos ? remap_spec.size() : comma;
    const std::string segment = remap_spec.substr(begin, end - begin);
    char bank[16]{};
    unsigned long long requested_offset = 0;
    unsigned long long requested_length = 0;
    unsigned long long replacement_offset = 0;
    if (std::sscanf(segment.c_str(), "%15[^:]:%llx:%llx:%llx", bank,
                    &requested_offset, &requested_length,
                    &replacement_offset) == 4) {
      const std::string expected_name = std::string(bank) + expected_suffix;
      const uint64_t request_end = requested_offset + requested_length;
      const uint64_t read_end = offset + length;
      if (logical_name == expected_name && requested_length != 0 &&
          request_end >= requested_offset && offset >= requested_offset &&
          read_end >= offset && read_end <= request_end &&
          replacement_offset <= UINT64_MAX - (offset - requested_offset)) {
        *source_offset = replacement_offset + (offset - requested_offset);
        return true;
      }
    }
    if (comma == std::string::npos) break;
    begin = comma + 1;
  }
  return false;
}

// XMemDecompressSegmentTD expands the DATA archive into a sparse logical
// address space.  DATA*.IDX stores the source page in 0x8000-byte units; the
// decoder writes each entry at raw_offset * 0x8000 in the .dec stream.  The
// guest, however, reads logical substreams whose offsets are not generally
// the IDX table's cumulative byte offsets.  The old bridge therefore needed
// hand-entered (offset,length -> decoded offset) guesses.  The opt-in bridge
// below learns those associations from unambiguous IDX sizes and keeps the
// inferred segments per XFile.
struct Tekken6TdIndexEntry {
  uint32_t index = 0;
  uint64_t decoded_start = 0;
  uint64_t size = 0;
};

struct Tekken6TdIndexTable {
  std::filesystem::path decoded_path;
  uint64_t decoded_size = 0;
  std::vector<Tekken6TdIndexEntry> entries;
};

std::mutex g_tekken6_td_index_cache_mutex;
std::unordered_map<std::string, std::shared_ptr<const Tekken6TdIndexTable>>
    g_tekken6_td_index_cache;

bool tek6_read_be32(const std::vector<uint8_t>& bytes, size_t offset, uint32_t* value) {
  if (offset > bytes.size() || bytes.size() - offset < 4) return false;
  *value = (uint32_t(bytes[offset]) << 24) |
           (uint32_t(bytes[offset + 1]) << 16) |
           (uint32_t(bytes[offset + 2]) << 8) | uint32_t(bytes[offset + 3]);
  return true;
}

std::shared_ptr<const Tekken6TdIndexTable> tek6_load_td_index(
    const std::filesystem::path& bin_path) {
  const std::filesystem::path decoded_path = [&] {
    auto p = bin_path;
    p.replace_extension(".dec");
    return p;
  }();
  const std::string cache_key = decoded_path.lexically_normal().string();

  {
    std::lock_guard<std::mutex> lock(g_tekken6_td_index_cache_mutex);
    auto it = g_tekken6_td_index_cache.find(cache_key);
    if (it != g_tekken6_td_index_cache.end()) return it->second;
  }

  auto table = std::make_shared<Tekken6TdIndexTable>();
  table->decoded_path = decoded_path;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(decoded_path, ec)) return nullptr;
  table->decoded_size = std::filesystem::file_size(decoded_path, ec);
  if (ec || table->decoded_size == 0) return nullptr;

  auto idx_path = bin_path;
  idx_path.replace_extension(".IDX");
  if (!std::filesystem::is_regular_file(idx_path, ec)) return nullptr;
  const uint64_t idx_size = std::filesystem::file_size(idx_path, ec);
  // IDX files are tiny (the largest local character bank is well below this),
  // and refusing a malformed/hostile file keeps this read path bounded.
  if (ec || idx_size == 0 || idx_size > 16 * 1024 * 1024 || (idx_size & 7) != 0) {
    return nullptr;
  }

  std::ifstream idx(idx_path, std::ios::binary);
  std::vector<uint8_t> bytes(static_cast<size_t>(idx_size));
  if (!idx.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()))) {
    return nullptr;
  }

  for (size_t pos = 0; pos < bytes.size(); pos += 8) {
    uint32_t raw_offset = 0;
    uint32_t entry_size = 0;
    if (!tek6_read_be32(bytes, pos, &raw_offset) ||
        !tek6_read_be32(bytes, pos + 4, &entry_size)) {
      return nullptr;
    }
    if (raw_offset == 0xffffffffu || entry_size == 0 || entry_size == 0xffffffffu) {
      continue;
    }
    const uint64_t decoded_start = uint64_t(raw_offset) * 0x8000ull;
    if (decoded_start > table->decoded_size ||
        uint64_t(entry_size) > table->decoded_size - decoded_start) {
      // A truncated decoded bank must never cause us to read the neighbouring
      // region.  Ignore this entry; if all useful entries are absent the
      // bridge simply falls back to the retail/raw path.
      continue;
    }
    table->entries.push_back({static_cast<uint32_t>(pos / 8), decoded_start,
                              uint64_t(entry_size)});
  }
  if (table->entries.empty()) return nullptr;

  std::sort(table->entries.begin(), table->entries.end(),
            [](const Tekken6TdIndexEntry& left, const Tekken6TdIndexEntry& right) {
              if (left.decoded_start != right.decoded_start) {
                return left.decoded_start < right.decoded_start;
              }
              return left.index < right.index;
            });

  {
    std::lock_guard<std::mutex> lock(g_tekken6_td_index_cache_mutex);
    auto [it, inserted] = g_tekken6_td_index_cache.emplace(cache_key, table);
    return inserted ? table : it->second;
  }
}

bool tek6_td_idx_source_offset(const std::filesystem::path& bin_path,
                               uint64_t logical_offset, uint32_t length,
                               std::vector<Tekken6TdVirtualSegment>& segments,
                               uint64_t* source_offset) {
  if (length == 0) return false;
  const auto table = tek6_load_td_index(bin_path);
  if (!table) return false;
  const uint64_t read_end = logical_offset + uint64_t(length);
  if (read_end < logical_offset) return false;

  auto add_segment = [&](const Tekken6TdIndexEntry& entry, uint64_t logical_start,
                         uint64_t logical_length) -> bool {
    if (logical_start > UINT64_MAX - logical_length ||
        entry.decoded_start > table->decoded_size ||
        uint64_t(length) > table->decoded_size - entry.decoded_start) {
      return false;
    }
    // Permit a request slightly larger than an IDX entry only when the
    // decoded file itself contains the complete requested range.  This is
    // needed for the observed DATA301 continuation read, whose request spans
    // the entry header and padding before the next 0x8000 page.
    const uint64_t usable_length = std::max(logical_length, uint64_t(length));
    if (entry.decoded_start > table->decoded_size - usable_length) return false;
    segments.push_back({logical_start, entry.decoded_start, usable_length, entry.index});
    *source_offset = entry.decoded_start + (logical_offset - logical_start);
    return *source_offset <= table->decoded_size - uint64_t(length);
  };

  // An exact-size match is the strongest evidence available at this layer.
  // Refuse duplicate sizes: guessing between repeated character records is
  // worse than retaining the raw path and makes the bridge non-diagnostic.
  const Tekken6TdIndexEntry* exact = nullptr;
  size_t exact_count = 0;
  for (const auto& entry : table->entries) {
    if (entry.size == uint64_t(length)) {
      exact = &entry;
      ++exact_count;
    }
  }
  if (exact_count == 1 && add_segment(*exact, logical_offset, exact->size)) {
    return true;
  }

  // Reuse the newest inferred segment before extending a stream.  New exact
  // anchors intentionally shadow older overlapping segments (DATA304 has
  // several independent logical substreams in one DATA*.BIN).
  for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
    if (logical_offset < it->logical_start ||
        logical_offset > UINT64_MAX - uint64_t(length) ||
        read_end > it->logical_start + it->length) {
      continue;
    }
    const uint64_t delta = logical_offset - it->logical_start;
    if (it->decoded_start > table->decoded_size - delta ||
        uint64_t(length) > table->decoded_size - it->decoded_start - delta) {
      continue;
    }
    *source_offset = it->decoded_start + delta;
    return true;
  }

  // If no unique size identifies the first request, offset zero is still a
  // safe anchor only for IDX entry zero.  This handles DATA302's partial read
  // of entry zero without mistaking one of its repeated sizes for another
  // entry.
  if (logical_offset == 0) {
    auto zero = std::find_if(table->entries.begin(), table->entries.end(),
                             [](const Tekken6TdIndexEntry& entry) {
                               return entry.decoded_start == 0;
                             });
    if (zero != table->entries.end() && add_segment(*zero, 0, zero->size)) {
      return true;
    }
  }

  // A request immediately after a known segment is commonly a short prefix
  // of the next physical IDX region.  Follow decoded physical order, but do
  // not bridge arbitrary gaps or unrelated reads.
  for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
    if (logical_offset != it->logical_start + it->length) continue;
    auto next = std::find_if(table->entries.begin(), table->entries.end(),
                             [&](const Tekken6TdIndexEntry& entry) {
                               return entry.decoded_start > it->decoded_start;
                             });
    if (next != table->entries.end() && add_segment(*next, logical_offset, next->size)) {
      return true;
    }
  }
  return false;
}

}  // namespace

XFile::XFile(KernelState* kernel_state, rex::filesystem::File* file, bool synchronous)
    : XObject(kernel_state, kObjectType), file_(file), is_synchronous_(synchronous) {
  async_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(async_event_);
}

XFile::XFile() : XObject(kObjectType) {
  async_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(async_event_);
}

XFile::~XFile() {
  // TODO(benvanik): signal that the file is closing?
  async_event_->Set();
  file_->Destroy();
}

uint64_t XFile::position() const {
  std::lock_guard<std::mutex> lock(file_lock_);
  return position_;
}

void XFile::set_position(uint64_t value) {
  std::lock_guard<std::mutex> lock(file_lock_);
  position_ = value;
}

X_STATUS XFile::QueryDirectory(X_FILE_DIRECTORY_INFORMATION* out_info, size_t length,
                               const std::string_view file_name, bool restart) {
  std::lock_guard<std::mutex> lock(file_lock_);
  assert_not_null(out_info);

  rex::filesystem::Entry* entry = nullptr;

  if (!file_name.empty()) {
    // Only queries in the current directory are supported for now.
    assert_true(rex::string::utf8_find_any_of(file_name, "\\") == std::string_view::npos);

    find_engine_.SetRule(file_name);

    // Always restart the search?
    find_index_ = 0;
    entry = file_->entry()->IterateChildren(find_engine_, &find_index_);
    if (!entry) {
      return X_STATUS_NO_SUCH_FILE;
    }
  } else {
    if (restart) {
      find_index_ = 0;
    }

    entry = file_->entry()->IterateChildren(find_engine_, &find_index_);
    if (!entry) {
      return X_STATUS_NO_MORE_FILES;
    }
  }

  auto end = reinterpret_cast<uint8_t*>(out_info) + length;
  const auto& entry_name = entry->name();
  if (reinterpret_cast<uint8_t*>(&out_info->file_name[0]) + entry_name.size() > end) {
    assert_always("Buffer overflow?");
    return X_STATUS_NO_SUCH_FILE;
  }

  out_info->next_entry_offset = 0;
  out_info->file_index = static_cast<uint32_t>(find_index_);
  out_info->creation_time = entry->create_timestamp();
  out_info->last_access_time = entry->access_timestamp();
  out_info->last_write_time = entry->write_timestamp();
  out_info->change_time = entry->write_timestamp();
  out_info->end_of_file = entry->size();
  out_info->allocation_size = entry->allocation_size();
  out_info->attributes = entry->attributes();
  out_info->file_name_length = static_cast<uint32_t>(entry_name.size());
  std::memcpy(out_info->file_name, entry_name.data(), entry_name.size());

  return X_STATUS_SUCCESS;
}

X_STATUS XFile::Read(uint32_t buffer_guest_address, uint32_t buffer_length, uint64_t byte_offset,
                     uint32_t* out_bytes_read, uint32_t apc_context, bool notify_completion) {
  std::lock_guard<std::mutex> lock(file_lock_);
  return ReadInternal(buffer_guest_address, buffer_length, byte_offset, out_bytes_read, apc_context,
                      notify_completion);
}

X_STATUS XFile::ReadInternal(uint32_t buffer_guest_address, uint32_t buffer_length,
                             uint64_t byte_offset, uint32_t* out_bytes_read, uint32_t apc_context,
                             bool notify_completion) {
  if (byte_offset == uint64_t(-1)) {
    // Read from current position.
    byte_offset = position_;
  }

  size_t bytes_read = 0;
  X_STATUS result = X_STATUS_SUCCESS;
  // Zero length means success for a valid file object according to Windows
  // tests.
  if (buffer_length) {
    if (UINT32_MAX - buffer_guest_address < buffer_length) {
      result = X_STATUS_ACCESS_VIOLATION;
    } else {
      // Games often read directly to texture/vertex buffer memory - in this
      // case, invalidation notifications must be sent. However, having any
      // memory callbacks in the range will result in STATUS_ACCESS_VIOLATION at
      // least on Windows, without anything being read or any callbacks being
      // triggered. So for physical memory, host protection must be bypassed,
      // and invalidation callbacks must be triggered manually (it's also wrong
      // to trigger invalidation callbacks before reading in this case, because
      // during the read, the guest may still access the data around the buffer
      // that is located in the same host pages as the buffer's start and end,
      // on the GPU - and that must not trigger a race condition).
      uint32_t buffer_guest_high_address = buffer_guest_address + buffer_length - 1;
      rex::memory::BaseHeap* buffer_start_heap = memory()->LookupHeap(buffer_guest_address);
      const rex::memory::BaseHeap* buffer_end_heap =
          memory()->LookupHeap(buffer_guest_high_address);
      if (!buffer_start_heap || !buffer_end_heap ||
          (buffer_start_heap->heap_type() == memory::HeapType::kGuestPhysical) !=
              (buffer_end_heap->heap_type() == memory::HeapType::kGuestPhysical) ||
          (buffer_start_heap->heap_type() == memory::HeapType::kGuestPhysical &&
           buffer_start_heap != buffer_end_heap)) {
        result = X_STATUS_ACCESS_VIOLATION;
      } else {
        rex::memory::PhysicalHeap* buffer_physical_heap =
            buffer_start_heap->heap_type() == memory::HeapType::kGuestPhysical
                ? static_cast<rex::memory::PhysicalHeap*>(buffer_start_heap)
                : nullptr;
        if (buffer_physical_heap && buffer_physical_heap->QueryRangeAccess(
                                        buffer_guest_address, buffer_guest_high_address) !=
                                        memory::PageAccess::kReadWrite) {
          result = X_STATUS_ACCESS_VIOLATION;
        } else {
          uint8_t* destination =
              buffer_physical_heap
                  ? memory()->TranslatePhysical(
                        buffer_physical_heap->GetPhysicalAddress(buffer_guest_address))
                  : memory()->TranslateVirtual(buffer_guest_address);
          uint64_t source_offset = byte_offset;
          bool decoded_overlay = false;
          std::string logical_name = name();
          const bool td_file = logical_name.size() == 11 &&
                               logical_name.rfind("DATA", 0) == 0 &&
                               logical_name.substr(7) == ".BIN";
          const bool remapped = td_file && tek6_td_source_offset(
              logical_name, byte_offset, buffer_length, &source_offset);
          const bool idx_bridge = td_file &&
                                  tek6_env_enabled("TEK6_RUNTIME_TD_IDX_BRIDGE");
          const auto* host_entry =
              dynamic_cast<const rex::filesystem::HostPathEntry*>(file_->entry());
          std::filesystem::path decoded_path;
          if (host_entry) {
            decoded_path = host_entry->host_path();
            decoded_path.replace_extension(".dec");
          }
          bool use_decoded_bank = false;
          if (host_entry && idx_bridge && !remapped) {
            uint64_t idx_source_offset = 0;
            if (tek6_td_idx_source_offset(host_entry->host_path(), byte_offset,
                                          buffer_length, tekken6_td_segments_,
                                          &idx_source_offset)) {
              source_offset = idx_source_offset;
              use_decoded_bank = true;
            }
          }
          if (host_entry && remapped && tek6_env_enabled("TEK6_RUNTIME_TD_BRIDGE")) {
            use_decoded_bank = true;
          }
          // Some extracted banks retain the guest's logical offsets in their
          // IDX records, while the raw .BIN is shorter than that logical
          // address space.  Keep this diagnostic and opt-in: only fall back
          // to the decoded companion when the raw request is provably beyond
          // the raw file but fully contained by the decoded bank.
          if (host_entry && !use_decoded_bank &&
              tek6_env_enabled("TEK6_RUNTIME_TD_DECODED_FALLBACK")) {
            const uint64_t raw_size = host_entry->size();
            std::error_code decoded_ec;
            const uint64_t decoded_size = std::filesystem::file_size(decoded_path, decoded_ec);
            const uint64_t request_end = source_offset + uint64_t(buffer_length);
            if (source_offset >= raw_size && request_end >= source_offset &&
                !decoded_ec && request_end <= decoded_size) {
              use_decoded_bank = true;
            }
          }
          if (use_decoded_bank) {
              std::error_code ec;
              if (std::filesystem::is_regular_file(decoded_path, ec)) {
                std::ifstream decoded(decoded_path, std::ios::binary);
                decoded.seekg(static_cast<std::streamoff>(source_offset), std::ios::beg);
                decoded.read(reinterpret_cast<char*>(destination), buffer_length);
                bytes_read = static_cast<size_t>(std::max<std::streamsize>(decoded.gcount(), 0));
                result = decoded.bad() ? X_STATUS_UNSUCCESSFUL : X_STATUS_SUCCESS;
                decoded_overlay = true;
              }
          }
          if (!decoded_overlay) {
            result = file_->ReadSync(std::span<uint8_t>(destination, buffer_length),
                                     size_t(source_offset), &bytes_read);
          }
          if (XSUCCEEDED(result)) {
            if (buffer_physical_heap) {
              buffer_physical_heap->TriggerCallbacks(
                  rex::thread::global_critical_region::AcquireDirect(), buffer_guest_address,
                  buffer_length, true, true);
            }

            if (byte_offset) {
              position_ = byte_offset;
            }
            position_ += bytes_read;
          }
        }
      }
    }
  }

  if (out_bytes_read) {
    *out_bytes_read = uint32_t(bytes_read);
  }

  if (notify_completion) {
    XIOCompletion::IONotification notify;
    notify.apc_context = apc_context;
    notify.num_bytes = uint32_t(bytes_read);
    notify.status = result;

    NotifyIOCompletionPorts(notify);

    async_event_->Set();
  }

  return result;
}

X_STATUS XFile::ReadScatter(uint32_t segments_guest_address, uint32_t length, uint64_t byte_offset,
                            uint32_t* out_bytes_read, uint32_t apc_context) {
  std::lock_guard<std::mutex> lock(file_lock_);
  X_STATUS result = X_STATUS_SUCCESS;

  // segments points to an array of buffer pointers of type
  // "FILE_SEGMENT_ELEMENT", but they can just be treated as normal pointers
  rex::be<uint32_t>* segments =
      reinterpret_cast<rex::be<uint32_t>*>(memory()->TranslateVirtual(segments_guest_address));

  // TODO: not sure if this is meant to change depending on buffer address?
  // (only game seen using this always seems to use 4096-byte buffers)
  uint32_t page_size = 4096;

  uint32_t read_total = 0;
  uint32_t read_remain = length;
  while (read_remain) {
    uint32_t read_length = read_remain;
    uint32_t read_buffer = *segments;
    if (read_length > page_size) {
      read_length = page_size;
      segments++;
    }

    uint32_t bytes_read = 0;
    result = ReadInternal(
        read_buffer, read_length,
        byte_offset
            ? ((byte_offset != -1 && byte_offset != -2) ? byte_offset + read_total : byte_offset)
            : -1,
        &bytes_read, apc_context, false);

    if (result != X_STATUS_SUCCESS) {
      break;
    }

    read_total += bytes_read;
    read_remain -= read_length;
  }

  if (out_bytes_read) {
    *out_bytes_read = uint32_t(read_total);
  }

  XIOCompletion::IONotification notify;
  notify.apc_context = apc_context;
  notify.num_bytes = uint32_t(read_total);
  notify.status = result;

  NotifyIOCompletionPorts(notify);

  async_event_->Set();

  return result;
}

X_STATUS XFile::Write(uint32_t buffer_guest_address, uint32_t buffer_length, uint64_t byte_offset,
                      uint32_t* out_bytes_written, uint32_t apc_context) {
  std::lock_guard<std::mutex> lock(file_lock_);
  if (byte_offset == uint64_t(-1)) {
    // Write from current position.
    byte_offset = position_;
  }

  size_t bytes_written = 0;
  X_STATUS result = file_->WriteSync(
      std::span<const uint8_t>(memory()->TranslateVirtual(buffer_guest_address), buffer_length),
      size_t(byte_offset), &bytes_written);
  if (XSUCCEEDED(result)) {
    position_ += bytes_written;
  }

  XIOCompletion::IONotification notify;
  notify.apc_context = apc_context;
  notify.num_bytes = uint32_t(bytes_written);
  notify.status = result;

  NotifyIOCompletionPorts(notify);

  if (out_bytes_written) {
    *out_bytes_written = uint32_t(bytes_written);
  }

  async_event_->Set();
  return result;
}

X_STATUS XFile::SetLength(size_t length) {
  std::lock_guard<std::mutex> lock(file_lock_);
  return file_->SetLength(length);
}

X_STATUS XFile::Rename(const std::filesystem::path& file_path) {
  std::lock_guard<std::mutex> lock(file_lock_);
  return file_->Rename(file_path);
}

void XFile::RegisterIOCompletionPort(uint32_t key, object_ref<XIOCompletion> port) {
  std::lock_guard<std::mutex> lock(completion_port_lock_);

  completion_ports_.push_back({key, port});
}

void XFile::RemoveIOCompletionPort(uint32_t key) {
  std::lock_guard<std::mutex> lock(completion_port_lock_);

  for (auto it = completion_ports_.begin(); it != completion_ports_.end(); it++) {
    if (it->first == key) {
      completion_ports_.erase(it);
      break;
    }
  }
}

bool XFile::Save(stream::ByteStream* stream) {
  REXSYS_DEBUG("XFile {:08X} ({})", handle(), file_->entry()->absolute_path().c_str());

  if (!SaveObject(stream)) {
    return false;
  }

  stream->Write(file_->entry()->absolute_path());
  stream->Write<uint64_t>(position_);
  stream->Write(file_access());
  stream->Write<bool>((file_->entry()->attributes() & rex::filesystem::kFileAttributeDirectory) !=
                      0);
  stream->Write<bool>(is_synchronous_);

  return true;
}

object_ref<XFile> XFile::Restore(KernelState* kernel_state, stream::ByteStream* stream) {
  auto file = new XFile();
  file->kernel_state_ = kernel_state;
  if (!file->RestoreObject(stream)) {
    delete file;
    return nullptr;
  }

  auto abs_path = stream->Read<std::string>();
  uint64_t position = stream->Read<uint64_t>();
  auto access = stream->Read<uint32_t>();
  auto is_directory = stream->Read<bool>();
  auto is_synchronous = stream->Read<bool>();

  REXSYS_DEBUG("XFile {:08X} ({})", file->handle(), abs_path);

  rex::filesystem::File* vfs_file = nullptr;
  rex::filesystem::FileAction action;
  auto res = kernel_state->file_system()->OpenFile(nullptr, abs_path,
                                                   rex::filesystem::FileDisposition::kOpen, access,
                                                   is_directory, false, &vfs_file, &action);
  if (XFAILED(res)) {
    REXSYS_ERROR("Failed to open XFile: error {:08X}", res);
    return object_ref<XFile>(file);
  }

  file->file_ = vfs_file;
  file->position_ = position;
  file->is_synchronous_ = is_synchronous;

  return object_ref<XFile>(file);
}

void XFile::NotifyIOCompletionPorts(XIOCompletion::IONotification& notification) {
  std::lock_guard<std::mutex> lock(completion_port_lock_);

  for (auto port : completion_ports_) {
    notification.key_context = port.first;
    port.second->QueueNotification(notification);
  }
}

}  // namespace rex::system
