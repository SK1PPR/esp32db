#pragma once
// meta.hpp — checkpoints in the "dbmeta" partition.
//
// Rule: nothing that changes on every write goes here (head, key count...).
// The log is self-describing, so those are rebuilt by replay. dbmeta only
// holds things that are expensive to rebuild and fine to be slightly stale:
//   - format version, so boot can refuse an incompatible layout
//   - later: index snapshot location + "replay only sectors with seq >= N"
//
// Stored as checkpoint records in a small flash::AppendLog of its own (16
// sectors) — the same class the data log uses, which is why these are
// ordinary classes and not singletons. Latest checkpoint = last valid record
// in the highest-seq sector. Old sectors are freed only after a newer
// checkpoint has been written, so a power cut always leaves one valid copy.

#include <cstdint>
#include "esp_err.h"
#include "flash/log.hpp"

namespace db {

constexpr uint32_t FORMAT_VERSION = 1;

struct Checkpoint {
    uint32_t magic;
    uint32_t format_version;
    uint32_t replay_from_seq;  // 0 = replay everything (no index snapshot yet)
    uint32_t crc;
};

class Meta {
public:
    // Opens dbmeta and loads the latest checkpoint. A blank dbmeta (first boot)
    // gets a fresh Checkpoint written.
    esp_err_t init();

    const Checkpoint &current() const { return cp_; }
    esp_err_t write(const Checkpoint &cp);

private:
    flash::AppendLog log_;
    Checkpoint cp_{};
};

}  // namespace db
