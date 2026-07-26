#pragma once

// Minimal in-memory ZIP builder shared by the mod tests. Fixtures are built at
// runtime rather than committed, so no hostile archive lives in the repo.

#include "miniz.h"
#include "miniz_zip.h"

#include <cstdint>
#include <string>
#include <vector>

class Zip {
public:
    Zip() { mz_zip_writer_init_heap(&m_z, 0, 64 * 1024); }
    ~Zip() { if (!m_done) mz_zip_writer_end(&m_z); }

    Zip(const Zip&) = delete;
    Zip& operator=(const Zip&) = delete;

    void add(const std::string& name, const std::string& data,
             int level = MZ_BEST_COMPRESSION) {
        mz_zip_writer_add_mem(&m_z, name.c_str(), data.data(), data.size(),
                              (mz_uint)level);
    }
    void add(const std::string& name, const std::vector<uint8_t>& data,
             int level = MZ_BEST_COMPRESSION) {
        mz_zip_writer_add_mem(&m_z, name.c_str(), data.data(), data.size(),
                              (mz_uint)level);
    }

    // finalize_heap_archive finalizes the archive itself and hands over
    // ownership of the block; finalizing separately first would make it fail.
    std::vector<uint8_t> finish() {
        void* buf = nullptr;
        size_t sz = 0;
        std::vector<uint8_t> out;
        if (mz_zip_writer_finalize_heap_archive(&m_z, &buf, &sz) && buf) {
            out.assign((uint8_t*)buf, (uint8_t*)buf + sz);
            mz_free(buf);
        } else {
            m_failed = true;
        }
        mz_zip_writer_end(&m_z);
        m_done = true;
        return out;
    }

    bool failed() const { return m_failed; }

private:
    mz_zip_archive m_z{};
    bool m_done = false;
    bool m_failed = false;
};
