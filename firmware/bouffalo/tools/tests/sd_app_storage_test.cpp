// SPDX-License-Identifier: Apache-2.0
// Compile the production backend against a fault-injecting FatFS fake.
#include "sd_app_storage.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {
struct Node {
    std::vector<uint8_t> bytes;
};
std::map<std::string, Node> files;
bool mounted = true, expand_denied = false;
std::string fault;
int syncs = 0, handles = 0;
bool fail(const char* name) {
    if (fault != name) return false;
    fault.clear();
    return true;
}
Node& node(FIL* f) { return *static_cast<Node*>(f->node); }
using Store = mpx_port::SdAppStorage;
using Error = micropixel::device::BlockStorageError;
void reset() {
    assert(handles == 0);
    files.clear();
    fault.clear();
    mounted = true;
    expand_denied = false;
}
}  // namespace
uint64_t bflb_mtimer_get_time_us() {
    static int64_t t;
    return ++t;
}
extern "C" {
bool sd_storage_mount() { return mounted; }
FSIZE_t mock_size(FIL* f) { return node(f).bytes.size(); }
FRESULT f_stat(const char* p, FILINFO* i) {
    auto f = files.find(p);
    if (f == files.end()) return FR_NO_FILE;
    i->fsize = f->second.bytes.size();
    i->fattrib = 0;
    return FR_OK;
}
FRESULT f_mkdir(const char*) { return FR_OK; }
FRESULT f_open(FIL* f, const char* p, uint8_t flags) {
    auto i = files.find(p);
    if (flags & FA_CREATE_NEW) {
        if (i != files.end()) return FR_EXIST;
        i = files.emplace(p, Node{}).first;
    } else if (i == files.end())
        return FR_NO_FILE;
    *f = {0, &i->second, nullptr};
    ++handles;
    return FR_OK;
}
FRESULT f_expand(FIL* f, FSIZE_t n, uint8_t) {
    if (fail("expand")) return FR_DISK_ERR;
    if (expand_denied) return FR_DENIED;
    node(f).bytes.resize(n, 0x5a);
    return FR_OK;
}
FRESULT f_lseek(FIL* f, FSIZE_t n) {
    if (n == CREATE_LINKMAP) return FR_DENIED;  // fragmented file fallback
    if (fail("seek")) return FR_DISK_ERR;
    if (n > node(f).bytes.size()) node(f).bytes.resize(n, 0x5a);
    f->pos = n;
    return FR_OK;
}
FRESULT f_sync(FIL*) {
    ++syncs;
    return fail("sync") ? FR_DISK_ERR : FR_OK;
}
FRESULT f_close(FIL* f) {
    assert(f->node);
    f->node = nullptr;
    --handles;
    return FR_OK;
}
FRESULT f_rename(const char* from, const char* to) {
    if (fail("rename")) return FR_DISK_ERR;
    if (files.contains(to)) return FR_EXIST;
    auto n = files.extract(from);
    if (n.empty()) return FR_NO_FILE;
    n.key() = to;
    files.insert(std::move(n));
    return FR_OK;
}
FRESULT f_read(FIL* f, void* p, UINT n, UINT* read) {
    assert(reinterpret_cast<uintptr_t>(p) % 64 == 0);
    if (fail("read")) return FR_DISK_ERR;
    *read = std::min<FSIZE_t>(n, node(f).bytes.size() - f->pos);
    if (fail("short_read") && *read) --*read;
    std::memcpy(p, node(f).bytes.data() + f->pos, *read);
    f->pos += *read;
    return FR_OK;
}
FRESULT f_write(FIL* f, const void* p, UINT n, UINT* written) {
    assert(reinterpret_cast<uintptr_t>(p) % 64 == 0);
    if (fail("write")) return FR_DISK_ERR;
    *written = n;
    if (fail("short_write") && n) --*written;
    assert(f->pos + *written <= node(f).bytes.size());
    std::memcpy(node(f).bytes.data() + f->pos, p, *written);
    f->pos += *written;
    return FR_OK;
}
}
int main() {
    Store s;
    reset();
    mounted = false;
    assert(!s.Initialize(true));
    assert(files.empty());
    assert(s.Close());
    reset();
    assert(!s.Initialize(false));
    assert(files.empty());
    assert(s.Close());
    files[Store::kPath].bytes = {1, 2, 3};
    assert(!s.Initialize(true));
    assert(files.at(Store::kPath).bytes == (std::vector<uint8_t>{1, 2, 3}));
    assert(s.Close());
    reset();
    files[Store::kPendingPath].bytes = {4, 5};
    assert(!s.Initialize(true));
    assert(files.at(Store::kPendingPath).bytes.size() == 2);
    assert(s.Close());
    reset();
    files[Store::kPendingPath].bytes = {4, 5};
    assert(!s.Initialize(true, true));
    assert(files.at(Store::kPendingPath).bytes.size() == 2);
    assert(s.Close());
    reset();
    files[Store::kPendingPath].bytes.resize(Store::kImageBytes, 0x73);
    assert(!s.Initialize(true));
    assert(s.Close());  // normal startup preserves it
    assert(s.Initialize(true, true));
    assert(s.pending());
    assert(files.at(Store::kPendingPath).bytes[0] == 0x73);  // opening never formats
    assert(s.Publish());
    assert(s.Close());
    reset();
    expand_denied = true;
    assert(s.Initialize(true));
    assert(s.pending());
    assert(s.geometry().size_bytes == Store::kImageBytes);
    assert(!s.Erase(1, 4096));
    assert(!s.Erase(0, 4095));
    std::array<uint8_t, 8192> out{};
    assert(!s.Read(UINT64_MAX, out));
    assert(!s.Program(Store::kImageBytes - 1, out));
    assert(s.Erase(0, 12288));
    assert(s.Read(0, out));
    assert(std::all_of(out.begin(), out.end(), [](auto b) { return b == 255; }));
    std::vector<uint8_t> data(8001);
    for (size_t i = 0; i < data.size(); ++i) data[i] = (i * 17) & 255;
    int before_sync = syncs;
    assert(s.Program(509, data));
    assert(syncs > before_sync);
    std::vector<uint8_t> result(data.size());
    assert(s.Read(509, result));
    assert(result == data);
    std::array<uint8_t, 4> marker{0x12, 0x34, 0x56, 0x78};
    assert(s.Program(10000, marker));
    std::array<uint8_t, 4> forbidden{255, 255, 255, 255};
    assert(s.Program(10000, forbidden).error() == Error::kInvalidArgument);
    assert(s.available());
    assert(s.Publish());
    assert(!s.pending());
    assert(!files.contains(Store::kPendingPath));
    assert(s.Close());
    assert(s.Initialize(false));
    assert(!s.pending());
    assert(s.Read(509, result));
    assert(result == data);
    assert(s.statistics().read_bytes == data.size());
    assert(s.Close());
    // An unrelated final image appearing before publication must be preserved.
    reset();
    assert(s.Initialize(true));
    files[Store::kPath].bytes = {99};
    assert(!s.Publish());
    assert(files.at(Store::kPath).bytes[0] == 99);
    assert(files.contains(Store::kPendingPath));
    assert(s.Close());
    reset();
    // Each IO error makes subsequent operations unavailable until reopened.
    assert(s.Initialize(true));
    assert(s.Erase(0, 4096));
    assert(s.Publish());
    assert(s.Close());
    for (auto kind : {"read", "short_read", "seek", "write", "short_write", "sync"}) {
        assert(s.Initialize(false));
        fault = kind;
        if (fault == "read" || fault == "short_read")
            assert(s.Read(0, out).error() == Error::kIo);
        else
            assert(s.Erase(0, 4096).error() == Error::kIo);
        assert(!s.available());
        assert(s.Read(0, out).error() == Error::kUnavailable);
        assert(s.Close());
    }
    for (auto kind : {"read", "short_read", "write", "short_write", "sync"}) {
        assert(s.Initialize(false));
        assert(s.Erase(0, 4096));
        fault = kind;
        assert(s.Program(0, marker).error() == Error::kIo);
        assert(!s.available());
        assert(s.Close());
    }
    assert(handles == 0);
    std::cout << "PASS: SD image protection, persistence, NOR semantics, bounds and IO failures\n";
}
