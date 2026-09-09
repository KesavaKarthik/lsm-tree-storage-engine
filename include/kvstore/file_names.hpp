#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace kvstore {

// Which data file a record lives in.
//
// The file's *name* is the id -- the id is deliberately not written into the
// record itself. Two reasons: it would cost 4 bytes on every record to store
// something the directory entry already says, and it would make a data file
// impossible to rename, which is exactly what compaction does when it installs
// a merged file over the highest input id.
//
// The consequence is that a ValuePointer must carry the file id in memory (see
// index.hpp): once there is more than one file, an offset alone is no longer a
// unique address.
using FileId = std::uint32_t;

// What the files in a database directory are called.
//
// A Bitcask directory:
//
//   000001.log        data file, id 1
//   000001.hint       index-only sidecar for data file 1 (compacted files only)
//   000001.log.tmp    compaction output, not yet installed
//
// An LSM directory:
//
//   MANIFEST          which tables live at which level -- the commit point
//   000003.wal        write-ahead log backing the current memtable
//   000001.sst        sorted string table, id 1 (higher id == newer)
//   000004.sst.tmp    flush or compaction output, not yet installed
//   MANIFEST.tmp      a level set being committed
//
// The two engines never share a directory, but they deliberately share this
// header: id padding, the temp-name convention and the "parse strictly, ignore
// what you don't understand" rule are properties of *this project's* on-disk
// naming, not of either engine. Two engines in one codebase disagreeing about
// what a filename means is exactly the bug this prevents.
//
// Zero-padded so `ls` sorts the way a human expects, but ids are always parsed
// and compared as *numbers*. Nothing may depend on the lexicographic order:
// it agrees with the numeric one only while the ids fit in kFileIdDigits, and
// silently stops agreeing at 1000000.
inline constexpr std::string_view kLogSuffix = ".log";
inline constexpr std::string_view kHintSuffix = ".hint";
inline constexpr std::string_view kTableSuffix = ".sst";
inline constexpr std::string_view kWalSuffix = ".wal";
inline constexpr std::string_view kTempSuffix = ".tmp";

// The only file here with a name rather than a number, because there is only
// ever one of it and its name has to be findable without reading anything else.
inline constexpr std::string_view kManifestName = "MANIFEST";
inline constexpr std::size_t kFileIdDigits = 6;

// Ids start at 1, so 0 can mean "no file" in a default-constructed pointer.
inline constexpr FileId kFirstFileId = 1;

namespace detail {

inline std::string pad_file_id(FileId id) {
    std::string digits = std::to_string(id);
    if (digits.size() < kFileIdDigits) {
        digits.insert(0, kFileIdDigits - digits.size(), '0');
    }
    return digits;
}

// Shared by the data-file and hint-file parsers: strip `suffix`, then read what
// is left as a decimal id.
inline std::optional<FileId> parse_id_with_suffix(std::string_view filename,
                                                  std::string_view suffix) {
    if (filename.size() <= suffix.size()) {
        return std::nullopt;
    }
    if (filename.substr(filename.size() - suffix.size()) != suffix) {
        return std::nullopt;
    }

    const std::string_view digits = filename.substr(0, filename.size() - suffix.size());
    std::uint64_t id = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        id = id * 10 + static_cast<std::uint64_t>(c - '0');
        if (id > std::numeric_limits<FileId>::max()) {
            return std::nullopt;  // Not a name we could ever have written.
        }
    }
    if (id < kFirstFileId) {
        return std::nullopt;
    }
    return static_cast<FileId>(id);
}

}  // namespace detail

// Names only, not paths: the caller has the directory and does `dir / name()`.
// Keeps <filesystem> out of this header and out of index.hpp, which includes it
// for FileId alone.
[[nodiscard]] inline std::string log_file_name(FileId id) {
    return detail::pad_file_id(id) + std::string{kLogSuffix};
}

[[nodiscard]] inline std::string hint_file_name(FileId id) {
    return detail::pad_file_id(id) + std::string{kHintSuffix};
}

// The LSM engine draws table ids and WAL ids from *one* counter, so a directory
// never holds a 000004.sst and a 000004.wal at the same time. That is not an
// economy -- it is what makes "higher id means newer" a single total order over
// everything the engine has ever written, which is the same rule Bitcask uses to
// decide which of two records wins.
[[nodiscard]] inline std::string sst_file_name(FileId id) {
    return detail::pad_file_id(id) + std::string{kTableSuffix};
}

[[nodiscard]] inline std::string wal_file_name(FileId id) {
    return detail::pad_file_id(id) + std::string{kWalSuffix};
}

// The in-progress name a file is written under before it is renamed into place.
[[nodiscard]] inline std::string temp_name(std::string_view final_name) {
    return std::string{final_name} + std::string{kTempSuffix};
}

// The id, or nullopt if this isn't one of our data files.
//
// Strict on purpose: recovery uses this to decide what is part of the database,
// so anything it doesn't fully understand must be ignored rather than guessed
// at. In particular "000001.log.tmp" does not parse -- an interrupted
// compaction's output is not a data file, and treating it as one would replay
// records nothing points at.
[[nodiscard]] inline std::optional<FileId> parse_log_file_id(std::string_view filename) {
    return detail::parse_id_with_suffix(filename, kLogSuffix);
}

[[nodiscard]] inline std::optional<FileId> parse_hint_file_id(std::string_view filename) {
    return detail::parse_id_with_suffix(filename, kHintSuffix);
}

[[nodiscard]] inline std::optional<FileId> parse_sst_file_id(std::string_view filename) {
    return detail::parse_id_with_suffix(filename, kTableSuffix);
}

[[nodiscard]] inline std::optional<FileId> parse_wal_file_id(std::string_view filename) {
    return detail::parse_id_with_suffix(filename, kWalSuffix);
}

// A file this engine was in the middle of writing when it stopped.
//
// Recognised narrowly -- it must be one of *our* names with .tmp on the end --
// because recovery deletes these, and deleting things in a directory somebody
// else may also be using deserves a precise rule.
[[nodiscard]] inline bool is_temp_name(std::string_view filename) {
    if (filename.size() <= kTempSuffix.size()) {
        return false;
    }
    if (filename.substr(filename.size() - kTempSuffix.size()) != kTempSuffix) {
        return false;
    }
    const std::string_view stem = filename.substr(0, filename.size() - kTempSuffix.size());
    return stem == kManifestName || parse_log_file_id(stem).has_value() ||
           parse_hint_file_id(stem).has_value() || parse_sst_file_id(stem).has_value() ||
           parse_wal_file_id(stem).has_value();
}

}  // namespace kvstore
