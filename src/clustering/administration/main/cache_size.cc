#include "clustering/administration/main/cache_size.hpp"

#include <inttypes.h>
#include <stddef.h>

#include <limits>
#include <string>

#include "arch/runtime/thread_pool.hpp"
#include "arch/types.hpp"
#include "logger.hpp"
#include "utils.hpp"

#ifndef __MACH__

size_t skip_spaces(const std::string &s, size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
        ++i;
    }
    return i;
}

bool parse_meminfo_line(const std::string &contents, size_t *offset_ref,
                        std::string *name_out,
                        uint64_t *value_out,
                        std::string *unit_out) {
    const size_t line_begin = *offset_ref;
    size_t line_end = contents.find('\n', line_begin);
    if (line_end == std::string::npos) {
        line_end = contents.size();
        *offset_ref = line_end;
    } else {
        *offset_ref = line_end + 1;
    }

    const std::string line = contents.substr(line_begin, line_end - line_begin);

    const size_t colon = line.find(':');
    if (colon == std::string::npos) {
        return false;
    }
    size_t i = skip_spaces(line, colon + 1);

    const size_t number_begin = i;
    while (i < line.size() && '0' <= line[i] && '9' >= line[i]) {
        ++i;
    }
    const size_t number_end = i;

    if (number_begin == number_end) {
        return false;
    }

    const size_t unit_begin = skip_spaces(line, i);
    size_t unit_end = line.find_first_of(" \t", unit_begin);
    if (unit_end == std::string::npos) {
        unit_end = line.size();
    }

    const size_t end = skip_spaces(line, unit_end);
    if (end != line.size()) {
        return false;
    }

    *name_out = line.substr(0, colon);
    if (!strtou64_strict(line.substr(number_begin, number_end - number_begin),
                         10,
                         value_out)) {
        return false;
    }
    *unit_out = line.substr(unit_begin, unit_end - unit_begin);
    return true;
}

bool parse_meminfo_file(const std::string &contents, uint64_t *mem_avail_out) {
    uint64_t memfree = 0;
    uint64_t cached = 0;

    bool seen_memfree = false;
    bool seen_cached = false;

    size_t offset = 0;
    std::string name;
    uint64_t value;
    std::string unit;
    while (parse_meminfo_line(contents, &offset, &name, &value, &unit)) {
        if (name == "MemFree") {
            if (seen_memfree) {
                return false;
            }
            if (unit != "kB") {
                return false;
            }
            seen_memfree = true;
            memfree = value * KILOBYTE;
        } else if (name == "Cached") {
            if (seen_cached) {
                return false;
            }
            if (unit != "kB") {
                return false;
            }
            seen_cached = true;
            cached = value * KILOBYTE;
        }

        if (seen_memfree && seen_cached) {
            break;
        }
    }

    if (seen_memfree && seen_cached) {
        *mem_avail_out = memfree + cached;
        return true;
    } else {
        return false;
    }
}

bool get_proc_meminfo_available_memory_size(uint64_t *mem_avail_out) {
    std::string contents;
    bool ok;
    thread_pool_t::run_in_blocker_pool([&]() {
        ok = blocking_read_file("/proc/meminfo", &contents);
    });
    if (!ok) {
        return false;
    }
    return parse_meminfo_file(contents, mem_avail_out);
}

#endif  // __MACH_

uint64_t get_avail_mem_size() {
    uint64_t page_size = sysconf(_SC_PAGESIZE);

#if defined(__MACH__)
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_statistics64_data_t vmstat;
    // We memset this struct to zero because of zero-knowledge paranoia that some old
    // system might use a shorter version of the struct, where it would not set the
    // vmstat.external_page_count field (which is relatively new) that we use below.
    // (Probably, instead, the host_statistics64 call will fail, because count would
    // be wrong.)
    memset(&vmstat, 0, sizeof(vmstat));
    if (KERN_SUCCESS != host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vmstat, &count)) {
        logERR("Could not determine available RAM for the default cache size "
            "(errno=%d).\n", get_errno());
        return 1024 * MEGABYTE;
    }
    // external_page_count is the number of pages that are file-backed (non-swap) --
    // see /usr/include/mach/vm_statistics.h, see also vm_stat.c, the implementation
    // of vm_stat, in Darwin.
    uint64_t ret = (vmstat.free_count + vmstat.external_page_count) * page_size;
    return ret;
#else
    {
        uint64_t memory;
        if (get_proc_meminfo_available_memory_size(&memory)) {
            return memory;
        } else {
            logERR("Could not parse /proc/meminfo, so we will treat cached file memory "
                "as if it were unavailable.");

            // This just returns what /proc/meminfo would report as "MemFree".
            uint64_t avail_mem_pages = sysconf(_SC_AVPHYS_PAGES);
            return avail_mem_pages * page_size;
        }
    }
#endif
}

uint64_t get_max_total_cache_size() {
    return std::numeric_limits<intptr_t>::max();
}

uint64_t get_default_total_cache_size() {
    const int64_t available_mem = get_avail_mem_size();

    // Default to half the available memory minus a gigabyte, to leave room for server
    // and query overhead, but never default to less than 100 megabytes
    const int64_t signed_res =
        std::min<int64_t>(available_mem - GIGABYTE, get_max_total_cache_size()) /
        DEFAULT_MAX_CACHE_RATIO;
    const uint64_t res = std::max<int64_t>(signed_res, 100 * MEGABYTE);
    return res;
}

bool validate_total_cache_size(uint64_t total_cache_size, std::string *error_out) {
    if (total_cache_size > get_max_total_cache_size()) {
        *error_out = strprintf("Requested cache size (%" PRIu64 " MB) is higher than "
            "the expected upper bound for this platform (%" PRIu64 " MB).",
            total_cache_size / static_cast<uint64_t>(MEGABYTE),
            get_max_total_cache_size() / static_cast<uint64_t>(MEGABYTE));
        return false;
    }
    return true;
}

void log_total_cache_size(uint64_t total_cache_size) {
    logINF("Using cache size of %" PRIu64 " MB",
       total_cache_size / static_cast<uint64_t>(MEGABYTE));

    /* Provide some warnings if the cache size or available memory seem inadequate. We
    can't *really* tell what could go wrong given that we don't know how much data or
    what kind of queries will be run, so these are just somewhat reasonable values. */
    const uint64_t available_memory = get_avail_mem_size();
    if (total_cache_size > available_memory) {
        logWRN("Requested cache size is larger than available memory.");
    } else if (total_cache_size + GIGABYTE > available_memory) {
        logWRN("Cache size does not leave much memory for server and query "
               "overhead (available memory: %" PRIu64 " MB).",
               available_memory / static_cast<uint64_t>(MEGABYTE));
    }
    if (total_cache_size <= 100 * MEGABYTE) {
        logWRN("Cache size is very low and may impact performance.");
    }
}
