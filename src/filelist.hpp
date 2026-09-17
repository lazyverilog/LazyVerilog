#pragma once

#include "config.hpp"

#include <filesystem>
#include <string>
#include <vector>

struct VcodeResult {
    std::vector<std::string> files;
    /// Byte size of each entry in `files`, in the same order, as seen while the
    /// filelist was read.  Zero for a path that could not be stat'd -- which is
    /// the same set the loader warns about.
    ///
    /// Carried out of the filelist pass so the background index queue can be
    /// ordered largest-first without a second metadata call per file.  It is a
    /// scheduling hint, not a fact anything depends on staying current.
    std::vector<uintmax_t> file_sizes;
    std::vector<std::string> include_dirs;
    /// Every filelist actually read, including the ones reached through `-f`,
    /// as normalized absolute paths.
    ///
    /// The client is asked to watch `.f` and `.vf`, so it reports a filelist
    /// edited by a branch switch or a generator -- and nothing used to know
    /// which paths those were, so the report was dropped and the project went
    /// on indexing the list as it stood at launch.  The loader already tracks
    /// this set to break `-f` cycles; carrying it out costs the copy.
    std::vector<std::string> filelists;
};

std::string resolve_vcode_path(const std::filesystem::path& root, const Config& config);
VcodeResult load_vcode(const std::filesystem::path& root, const Config& config);

