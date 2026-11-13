// Minimal SquashFS 4.0 reader / extractor. See squashfs.h.
#include "squashfs.h"
#include "hostfs.h"
#include <zlib.h>
#include <xz.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <functional>
#include <algorithm>
#ifdef _WIN32
#include "winutil.h"
#endif

namespace sqfs {
namespace {

#pragma pack(push, 1)
struct Super {
    u32 magic, inode_count, mod_time, block_size, fragment_count;
    u16 compression, block_log, flags, id_count, ver_major, ver_minor;
    u64 root_inode, bytes_used, id_table, xattr_table, inode_table, dir_table, frag_table, export_table;
};
#pragma pack(pop)
static_assert(sizeof(Super) == 96, "squashfs superblock");

enum { COMP_GZIP = 1, COMP_XZ = 4 };

struct Inode {
    u16 type = 0;
    u16 mode = 0;
    // directory
    u32 dir_block = 0, dir_size = 0;
    u16 dir_offset = 0;
    // file
    u64 blocks_start = 0, file_size = 0;
    u32 frag = 0xffffffffu, frag_offset = 0;
    std::vector<u32> block_sizes;
    // symlink
    std::string target;
};

class Image {
public:
    ~Image() {
        if (f_) fclose(f_);
    }
    bool open(const std::string& path, std::string& err) {
#ifdef _WIN32
        f_ = _wfopen(utf8_to_wide(path).c_str(), L"rb");
#else
        f_ = fopen(path.c_str(), "rb");
#endif
        if (!f_) return fail(err, "cannot open " + path);
        if (!read_at(0, &sb_, sizeof sb_)) return fail(err, "cannot read superblock");
        if (sb_.magic != 0x73717368) return fail(err, "not a squashfs image");
        if (sb_.ver_major != 4) return fail(err, "unsupported squashfs version");
        if (sb_.compression != COMP_GZIP && sb_.compression != COMP_XZ)
            return fail(err, "unsupported squashfs compression " + std::to_string(sb_.compression) + " (gzip and xz are supported)");
        return true;
    }
    const Super& sb() const { return sb_; }

    bool read_inode(u64 ref, Inode& ino) {
        u64 blk = sb_.inode_table + (ref >> 16);
        u32 off = (u32)(ref & 0xffff);
        u8 hdr[16];
        if (!meta_read(blk, off, hdr, 16)) return false;
        memcpy(&ino.type, hdr, 2);
        memcpy(&ino.mode, hdr + 2, 2);
        auto rd = [&](void* p, size_t n) { return meta_read(blk, off, p, n); };
        switch (ino.type) {
        case 1: {  // basic dir
            u32 start, nlink, parent;
            u16 size, boff;
            if (!rd(&start, 4) || !rd(&nlink, 4) || !rd(&size, 2) || !rd(&boff, 2) || !rd(&parent, 4)) return false;
            ino.dir_block = start;
            ino.dir_size = size;
            ino.dir_offset = boff;
            return true;
        }
        case 8: {  // extended dir
            u32 nlink, size, start, parent, xattr;
            u16 icount, boff;
            if (!rd(&nlink, 4) || !rd(&size, 4) || !rd(&start, 4) || !rd(&parent, 4) || !rd(&icount, 2) || !rd(&boff, 2) || !rd(&xattr, 4))
                return false;
            ino.dir_block = start;
            ino.dir_size = size;
            ino.dir_offset = boff;
            return true;
        }
        case 2: case 9: {  // basic / extended file
            if (ino.type == 2) {
                u32 start, frag, foff, size;
                if (!rd(&start, 4) || !rd(&frag, 4) || !rd(&foff, 4) || !rd(&size, 4)) return false;
                ino.blocks_start = start;
                ino.frag = frag;
                ino.frag_offset = foff;
                ino.file_size = size;
            } else {
                u64 start, size, sparse;
                u32 nlink, frag, foff, xattr;
                if (!rd(&start, 8) || !rd(&size, 8) || !rd(&sparse, 8) || !rd(&nlink, 4) || !rd(&frag, 4) || !rd(&foff, 4) || !rd(&xattr, 4))
                    return false;
                ino.blocks_start = start;
                ino.frag = frag;
                ino.frag_offset = foff;
                ino.file_size = size;
            }
            u64 bs = sb_.block_size;
            u64 nblocks = ino.frag == 0xffffffffu ? (ino.file_size + bs - 1) / bs : ino.file_size / bs;
            ino.block_sizes.resize((size_t)nblocks);
            return nblocks == 0 || rd(ino.block_sizes.data(), (size_t)nblocks * 4);
        }
        case 3: case 10: {  // symlink
            u32 nlink, tsize;
            if (!rd(&nlink, 4) || !rd(&tsize, 4) || tsize > 65536) return false;
            ino.target.resize(tsize);
            return rd(ino.target.data(), tsize);
        }
        default:
            return true;  // devices, fifos, sockets: header is enough
        }
    }

    // Directory entries: name -> (inode ref, type)
    bool read_dir(const Inode& d, std::vector<std::tuple<std::string, u64, u16>>& out) {
        if (d.dir_size <= 3) return true;
        u64 blk = sb_.dir_table + d.dir_block;
        u32 off = d.dir_offset;
        s64 left = (s64)d.dir_size - 3;
        while (left > 0) {
            u32 count, start, inum;
            if (!meta_read(blk, off, &count, 4) || !meta_read(blk, off, &start, 4) || !meta_read(blk, off, &inum, 4)) return false;
            left -= 12;
            for (u32 i = 0; i <= count && left > 0; i++) {
                u16 ioff, type, nsize;
                s16 idelta;
                if (!meta_read(blk, off, &ioff, 2) || !meta_read(blk, off, &idelta, 2) || !meta_read(blk, off, &type, 2) ||
                    !meta_read(blk, off, &nsize, 2))
                    return false;
                std::string name(nsize + 1u, '\0');
                if (!meta_read(blk, off, name.data(), name.size())) return false;
                left -= 8 + (s64)name.size();
                out.emplace_back(name, ((u64)start << 16) | ioff, type);
            }
        }
        return true;
    }

    // Stream a file's contents to `sink`.
    bool read_file(const Inode& ino, const std::function<bool(const u8*, size_t)>& sink, std::string& err) {
        u64 pos = ino.blocks_start;
        u64 left = ino.file_size;
        u32 bs = sb_.block_size;
        std::vector<u8> out(bs), raw;
        for (u32 v : ino.block_sizes) {
            size_t want = (size_t)std::min<u64>(left, bs);
            u32 csize = v & 0xffffff;
            bool uncompressed = v & 0x1000000;
            if (csize == 0) {  // sparse
                std::fill(out.begin(), out.begin() + want, 0);
            } else {
                raw.resize(csize);
                if (!read_at(pos, raw.data(), csize)) return fail(err, "short read in data block");
                if (uncompressed) memcpy(out.data(), raw.data(), std::min<size_t>(csize, want));
                else if (decompress(raw.data(), csize, out.data(), bs) < want) return fail(err, "data block decompression failed");
                pos += csize;
            }
            if (!sink(out.data(), want)) return fail(err, "write failed");
            left -= want;
        }
        if (left && ino.frag != 0xffffffffu) {
            const std::vector<u8>* fb = fragment(ino.frag);
            if (!fb || ino.frag_offset + left > fb->size()) return fail(err, "bad fragment");
            if (!sink(fb->data() + ino.frag_offset, (size_t)left)) return fail(err, "write failed");
        }
        return true;
    }

private:
    FILE* f_ = nullptr;
    Super sb_{};
    std::map<u64, std::pair<std::vector<u8>, u64>> meta_cache_;  // block pos -> (data, next block pos)
    std::map<u32, std::vector<u8>> frag_cache_;
    std::vector<u64> frag_index_;

    static bool fail(std::string& err, const std::string& m) {
        err = m;
        return false;
    }

    bool read_at(u64 off, void* p, size_t n) {
#ifdef _WIN32
        if (_fseeki64(f_, (s64)off, SEEK_SET) != 0) return false;
#else
        if (fseeko(f_, (off_t)off, SEEK_SET) != 0) return false;
#endif
        return fread(p, 1, n, f_) == n;
    }

    size_t decompress(const u8* in, size_t n, u8* out, size_t cap) {
        if (sb_.compression == COMP_GZIP) {
            uLongf dl = (uLongf)cap;
            z_stream zs{};
            zs.next_in = (Bytef*)in;
            zs.avail_in = (uInt)n;
            zs.next_out = out;
            zs.avail_out = (uInt)cap;
            if (inflateInit(&zs) != Z_OK) return 0;
            int r = inflate(&zs, Z_FINISH);
            dl = zs.total_out;
            inflateEnd(&zs);
            return (r == Z_STREAM_END || r == Z_OK || r == Z_BUF_ERROR) ? (size_t)dl : 0;
        }
        static bool crc_init = [] {
            xz_crc32_init();
            xz_crc64_init();
            return true;
        }();
        (void)crc_init;
        xz_dec* d = xz_dec_init(XZ_SINGLE, 0);
        if (!d) return 0;
        xz_buf b{};
        b.in = in;
        b.in_size = n;
        b.out = out;
        b.out_size = cap;
        xz_ret r = xz_dec_run(d, &b);
        xz_dec_end(d);
        return (r == XZ_STREAM_END || r == XZ_OK) ? b.out_pos : 0;
    }

    const std::pair<std::vector<u8>, u64>* meta_block(u64 pos) {
        auto it = meta_cache_.find(pos);
        if (it != meta_cache_.end()) return &it->second;
        u16 hdr;
        if (!read_at(pos, &hdr, 2)) return nullptr;
        u32 size = hdr & 0x7fff;
        std::vector<u8> raw(size), data;
        if (!read_at(pos + 2, raw.data(), size)) return nullptr;
        if (hdr & 0x8000) data = std::move(raw);
        else {
            data.resize(8192);
            size_t got = decompress(raw.data(), size, data.data(), data.size());
            if (!got) return nullptr;
            data.resize(got);
        }
        if (meta_cache_.size() > 4096) meta_cache_.clear();
        auto& e = meta_cache_[pos];
        e = {std::move(data), pos + 2 + size};
        return &e;
    }

    // Read n bytes of a metadata stream, advancing (blk, off) across block boundaries.
    bool meta_read(u64& blk, u32& off, void* p, size_t n) {
        u8* dst = (u8*)p;
        while (n) {
            auto* b = meta_block(blk);
            if (!b) return false;
            if (off >= b->first.size()) {
                off -= (u32)b->first.size();
                blk = b->second;
                continue;
            }
            size_t c = std::min<size_t>(n, b->first.size() - off);
            memcpy(dst, b->first.data() + off, c);
            dst += c;
            n -= c;
            off += (u32)c;
            if (off == b->first.size() && n) {
                off = 0;
                blk = b->second;
            }
        }
        return true;
    }

    const std::vector<u8>* fragment(u32 idx) {
        auto it = frag_cache_.find(idx);
        if (it != frag_cache_.end()) return &it->second;
        if (frag_index_.empty() && sb_.fragment_count) {
            size_t nidx = (sb_.fragment_count + 511) / 512;
            frag_index_.resize(nidx);
            if (!read_at(sb_.frag_table, frag_index_.data(), nidx * 8)) return nullptr;
        }
        if (idx >= sb_.fragment_count) return nullptr;
        u64 blk = frag_index_[idx / 512];
        u32 off = (idx % 512) * 16;
        u64 start;
        u32 size, unused;
        if (!meta_read(blk, off, &start, 8) || !meta_read(blk, off, &size, 4) || !meta_read(blk, off, &unused, 4)) return nullptr;
        u32 csize = size & 0xffffff;
        std::vector<u8> raw(csize), out;
        if (!read_at(start, raw.data(), csize)) return nullptr;
        if (size & 0x1000000) out = std::move(raw);
        else {
            out.resize(sb_.block_size);
            size_t got = decompress(raw.data(), csize, out.data(), out.size());
            if (!got) return nullptr;
            out.resize(got);
        }
        if (frag_cache_.size() > 64) frag_cache_.clear();
        return &(frag_cache_[idx] = std::move(out));
    }
};

using Visitor = std::function<bool(const std::string& path, const Inode& ino, u64 ref)>;

// Depth-first walk; visitor returns false to skip a directory's subtree.
bool walk(Image& img, u64 ref, const std::string& path, const Visitor& v, std::string& err, int depth = 0) {
    if (depth > 256) {
        err = "directory nesting too deep";
        return false;
    }
    Inode ino;
    if (!img.read_inode(ref, ino)) {
        err = "cannot read inode for " + (path.empty() ? std::string("/") : path);
        return false;
    }
    if (!path.empty() && !v(path, ino, ref)) return true;
    if (ino.type != 1 && ino.type != 8) return true;
    std::vector<std::tuple<std::string, u64, u16>> ents;
    if (!img.read_dir(ino, ents)) {
        err = "cannot read directory " + path;
        return false;
    }
    for (auto& [name, cref, type] : ents)
        if (!walk(img, cref, path + "/" + name, v, err, depth + 1)) return false;
    return true;
}

bool under(const std::string& p, const std::string& root) {
    return p == root || (p.size() > root.size() && p.compare(0, root.size(), root) == 0 && p[root.size()] == '/');
}

}  // namespace

bool list(const std::string& image, std::vector<std::string>& paths, std::string& err) {
    Image img;
    if (!img.open(image, err)) return false;
    return walk(img, img.sb().root_inode, "", [&](const std::string& p, const Inode&, u64) {
        paths.push_back(p);
        return true;
    }, err);
}

bool extract(const std::string& image, const std::string& dest, const std::vector<std::string>& exclude,
             const std::string& only, std::string& err, const std::function<void()>& progress) {
    Image img;
    if (!img.open(image, err)) return false;
    auto mkdirs = [](const std::string& path) {
        // create every missing parent of path (path itself included)
        for (size_t i = 1; i <= path.size(); i++)
            if (i == path.size() || path[i] == '/') {
                std::string d = path.substr(0, i);
                if (d.size() > 2 && !hfs::exists(d)) hfs::mkdir(d, 0755);
            }
    };
    mkdirs(dest);
    bool ok = walk(img, img.sb().root_inode, "", [&](const std::string& p, const Inode& ino, u64) {
        if (progress) progress();
        for (auto& x : exclude)
            if (under(p, x)) return false;
        if (!only.empty() && !under(p, only) && !under(only, p)) return false;
        bool want = only.empty() || under(p, only);
        std::string host = dest + p;
        switch (ino.type) {
        case 1: case 8:
            if (want && !hfs::exists(host)) hfs::mkdir(host, 0755);
            return true;
        case 2: case 9: {
            if (!want) return true;
            mkdirs(host.substr(0, host.rfind('/')));
            hfs::unlink(host);
            int fd = hfs::open(host, lnx::L_O_CREAT | lnx::L_O_TRUNC | 1u, 0755);
            if (fd < 0) {
                fprintf(stderr, "  cannot create %s\n", p.c_str());
                return true;
            }
            std::string e;
            bool good = img.read_file(ino, [&](const u8* d, size_t n) {
                size_t done = 0;
                while (done < n) {
                    s64 w = hfs::write(fd, d + done, (u32)std::min<size_t>(n - done, 1u << 24), false);
                    if (w <= 0) return false;
                    done += (size_t)w;
                }
                return true;
            }, e);
            hfs::close(fd);
            if (!good) fprintf(stderr, "  %s: %s\n", p.c_str(), e.c_str());
            return true;
        }
        case 3: case 10:
            if (!want) return true;
            mkdirs(host.substr(0, host.rfind('/')));
            hfs::unlink(host);
            hfs::symlink(ino.target, host);
            return true;
        case 6: case 13:
            if (!want) return true;
            hfs::unlink(host);
            hfs::mkfifo(host, 0666);
            return true;
        default:
            return true;  // device nodes and sockets: provided by the emulator
        }
    }, err);
    return ok;
}

}  // namespace sqfs
