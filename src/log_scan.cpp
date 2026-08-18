#include "log_scan.hpp"

namespace kvstore {

Result<std::uint64_t> scan_records(const LogFile& file, const RecordVisitor& visit) {
    const std::uint64_t file_size = file.size();
    std::uint64_t offset = 0;
    std::uint64_t good_end = 0;  // End of the last record that fully verified.

    while (offset + record::kHeaderSize <= file_size) {
        auto header_bytes = file.read_at(offset, static_cast<std::uint32_t>(record::kHeaderSize));
        if (!header_bytes.is_ok()) {
            break;  // Can't even read a header: the tail starts here.
        }

        auto header = record::decode_header(*header_bytes);
        if (!header.is_ok()) {
            break;  // Mis-shaped header -- garbage from a partial write.
        }

        const std::uint64_t total = header->total_size();

        // Bound the declared sizes against the real file *before* trusting
        // them. A corrupt header can claim a 4GB key; without this check we
        // would try to allocate and read it. This is also what catches the
        // ordinary torn tail, where the header survived but the body didn't.
        if (offset + total > file_size) {
            break;
        }

        // The cast cannot truncate: decode_header rejects any total above
        // record::kMaxRecordSize, which is exactly UINT32_MAX. Without that
        // check a corrupt header on a >4GB file could pass the bound above and
        // then be silently narrowed here.
        auto bytes = file.read_at(offset, static_cast<std::uint32_t>(total));
        if (!bytes.is_ok()) {
            break;
        }

        auto rec = record::decode(*bytes);
        if (!rec.is_ok()) {
            break;  // crc mismatch: this record was never finished.
        }

        // A visitor failure is not a corrupt file -- it is the caller's own I/O
        // going wrong (compaction failing to write the record out, say). It
        // propagates as an error rather than being reported as a short prefix,
        // which would look to the caller like damage that isn't there.
        KVSTORE_RETURN_IF_ERROR(visit(offset, *rec, *bytes));

        offset += total;
        good_end = offset;
    }

    return good_end;
}

}  // namespace kvstore
