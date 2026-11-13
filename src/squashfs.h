// Minimal SquashFS 4.0 reader / extractor (gzip and XZ compression), used where unsquashfs is not
// available (Windows). Files are written through hostfs, so symlinks and FIFOs become hostfs
// markers and names with characters the host forbids are mapped transparently.
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace sqfs {
// All entry paths in the image, "/"-rooted (like `unsquashfs -l -d ''`).
bool list(const std::string& image, std::vector<std::string>& paths, std::string& err);
// Extract into dest. Entries in `exclude` (and their subtrees) are skipped; if `only` is non-empty,
// just that entry (with its subtree) is extracted, keeping its full path under dest.
// `progress` (optional) is called once per image entry visited.
bool extract(const std::string& image, const std::string& dest, const std::vector<std::string>& exclude,
             const std::string& only, std::string& err, const std::function<void()>& progress = {});
}  // namespace sqfs
