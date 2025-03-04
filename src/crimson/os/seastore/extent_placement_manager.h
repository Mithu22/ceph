// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 smarttab expandtab

#pragma once

#include "seastar/core/gate.hh"

#include "crimson/common/condition_variable.h"
#include "crimson/os/seastore/logging.h"
#include "crimson/os/seastore/cache.h"
#include "crimson/os/seastore/cached_extent.h"
#include "crimson/os/seastore/lba_manager.h"

namespace crimson::os::seastore {

/**
 * ool_record_t
 *
 * Encapsulates logic for building and encoding an ool record destined for
 * an ool segment.
 *
 * Uses a metadata header to enable scanning the ool segment for gc purposes.
 * Introducing a seperate physical->logical mapping would enable removing the
 * metadata block overhead.
 */
class ool_record_t {
  class OolExtent {
  public:
    OolExtent(LogicalCachedExtentRef& lextent)
      : lextent(lextent) {}
    void set_ool_paddr(paddr_t addr) {
      ool_offset = addr;
    }
    paddr_t get_ool_paddr() const {
      return ool_offset;
    }
    bufferptr& get_bptr() {
      return lextent->get_bptr();
    }
    LogicalCachedExtentRef& get_lextent() {
      return lextent;
    }
  private:
    paddr_t ool_offset;
    LogicalCachedExtentRef lextent;
  };

public:
  ool_record_t(size_t block_size) : block_size(block_size) {}
  record_size_t get_encoded_record_length() {
    return crimson::os::seastore::get_encoded_record_length(record, block_size);
  }
  size_t get_wouldbe_encoded_record_length(LogicalCachedExtentRef& extent) {
    auto raw_mdlength = get_encoded_record_raw_mdlength(record, block_size);
    auto wouldbe_mdlength = p2roundup(
      raw_mdlength + ceph::encoded_sizeof_bounded<extent_info_t>(),
      block_size);
    return wouldbe_mdlength + extent_buf_len + extent->get_bptr().length();
  }
  ceph::bufferlist encode(segment_id_t segment, segment_nonce_t nonce) {
    assert(extents.size() == record.extents.size());
    auto rsize = get_encoded_record_length();
    segment_off_t extent_offset = base + rsize.mdlength;
    for (auto& extent : extents) {
      extent.set_ool_paddr(
        {segment, extent_offset});
      extent_offset += extent.get_bptr().length();
    }
    assert(extent_offset == (segment_off_t)(base + rsize.mdlength + rsize.dlength));
    return encode_record(rsize, std::move(record), block_size, base, nonce);
  }
  void add_extent(LogicalCachedExtentRef& extent) {
    extents.emplace_back(extent);
    ceph::bufferlist bl;
    bl.append(extent->get_bptr());
    record.extents.emplace_back(extent_t{
      extent->get_type(),
      extent->get_laddr(),
      std::move(bl)});
    extent_buf_len += extent->get_bptr().length();
  }
  std::vector<OolExtent>& get_extents() {
    return extents;
  }
  void set_base(segment_off_t b) {
    base = b;
  }
  segment_off_t get_base() {
    return base;
  }
  void clear() {
    record.extents.clear();
    extents.clear();
    assert(!record.deltas.size());
    extent_buf_len = 0;
    base = MAX_SEG_OFF;
  }
  uint64_t get_num_extents() const {
    return extents.size();
  }
  uint64_t get_raw_data_size() const {
    assert(extents.size() == record.extents.size());
    return record.get_raw_data_size();
  }
private:
  std::vector<OolExtent> extents;
  record_t record;
  size_t block_size;
  segment_off_t extent_buf_len = 0;
  segment_off_t base = MAX_SEG_OFF;
};

/**
 * ExtentOolWriter
 *
 * Interface through which final write to ool segment is performed.
 */
class ExtentOolWriter {
public:
  using write_iertr = trans_iertr<crimson::errorator<
    crimson::ct_error::input_output_error, // media error or corruption
    crimson::ct_error::invarg,             // if offset is < write pointer or misaligned
    crimson::ct_error::ebadf,              // segment closed
    crimson::ct_error::enospc              // write exceeds segment size
    >>;

  using stop_ertr = Segment::close_ertr;
  virtual stop_ertr::future<> stop() = 0;
  virtual write_iertr::future<> write(
    Transaction& t,
    std::list<LogicalCachedExtentRef>& extent) = 0;
  virtual ~ExtentOolWriter() {}
};

/**
 * ExtentAllocator
 *
 * Handles allocating ool extents from a specific family of targets.
 */
class ExtentAllocator {
public:
  using alloc_paddr_iertr = trans_iertr<crimson::errorator<
    crimson::ct_error::input_output_error, // media error or corruption
    crimson::ct_error::invarg,             // if offset is < write pointer or misaligned
    crimson::ct_error::ebadf,              // segment closed
    crimson::ct_error::enospc              // write exceeds segment size
    >>;

  virtual alloc_paddr_iertr::future<> alloc_ool_extents_paddr(
    Transaction& t,
    std::list<LogicalCachedExtentRef>&) = 0;

  using stop_ertr = ExtentOolWriter::stop_ertr;
  virtual stop_ertr::future<> stop() = 0;
  virtual ~ExtentAllocator() {};
};
using ExtentAllocatorRef = std::unique_ptr<ExtentAllocator>;

struct open_segment_wrapper_t : public boost::intrusive_ref_counter<
  open_segment_wrapper_t,
  boost::thread_unsafe_counter> {
  SegmentRef segment;
  std::list<seastar::future<>> inflight_writes;
  bool outdated = false;
};

using open_segment_wrapper_ref =
  boost::intrusive_ptr<open_segment_wrapper_t>;

/**
 * SegmentedAllocator
 *
 * Handles out-of-line writes to a SegmentManager device (such as a ZNS device
 * or conventional flash device where sequential writes are heavily preferred).
 *
 * Creates <seastore_init_rewrite_segments_per_device> Writer instances
 * internally to round-robin writes.  Later work will partition allocations
 * based on hint (age, presumably) among the created Writers.

 * Each Writer makes use of SegmentProvider to obtain a new segment for writes
 * as needed.
 */
class SegmentedAllocator : public ExtentAllocator {
  class Writer : public ExtentOolWriter {
  public:
    Writer(
      SegmentProvider& sp,
      SegmentManager& sm,
      LBAManager& lba_manager,
      Journal& journal,
      Cache& cache)
      : segment_provider(sp),
        segment_manager(sm),
        lba_manager(lba_manager),
        journal(journal),
        cache(cache)
    {}
    Writer(Writer &&) = default;

    write_iertr::future<> write(
      Transaction& t,
      std::list<LogicalCachedExtentRef>& extent) final;
    stop_ertr::future<> stop() final {
      return writer_guard.close().then([this] {
        return crimson::do_for_each(open_segments, [](auto& seg_wrapper) {
          return seg_wrapper->segment->close();
        });
      });
    }
  private:
    using update_lba_mapping_iertr = LBAManager::update_le_mapping_iertr;
    using finish_record_iertr = update_lba_mapping_iertr;
    using finish_record_ret = finish_record_iertr::future<>;
    finish_record_ret finish_write(
      Transaction& t,
      ool_record_t& record);
    bool _needs_roll(segment_off_t length) const;

    write_iertr::future<> _write(
      Transaction& t,
      ool_record_t& record);

    using roll_segment_ertr = crimson::errorator<
      crimson::ct_error::input_output_error>;
    roll_segment_ertr::future<> roll_segment(bool);

    using init_segment_ertr = crimson::errorator<
      crimson::ct_error::input_output_error>;
    init_segment_ertr::future<> init_segment(Segment& segment);

    void add_extent_to_write(
      ool_record_t&,
      LogicalCachedExtentRef& extent);

    SegmentProvider& segment_provider;
    SegmentManager& segment_manager;
    open_segment_wrapper_ref current_segment;
    std::list<open_segment_wrapper_ref> open_segments;
    segment_off_t allocated_to = 0;
    LBAManager& lba_manager;
    Journal& journal;
    crimson::condition_variable segment_rotation_guard;
    seastar::gate writer_guard;
    bool rolling_segment = false;
    Cache& cache;
  };
public:
  SegmentedAllocator(
    SegmentProvider& sp,
    SegmentManager& sm,
    LBAManager& lba_manager,
    Journal& journal,
    Cache& cache);

  Writer &get_writer(ool_placement_hint_t hint) {
    return writers[std::rand() % writers.size()];
  }

  alloc_paddr_iertr::future<> alloc_ool_extents_paddr(
    Transaction& t,
    std::list<LogicalCachedExtentRef>& extents) final {
    LOG_PREFIX(SegmentedAllocator::alloc_ool_extents_paddr);
    DEBUGT("start", t);
    return seastar::do_with(
      std::map<Writer*, std::list<LogicalCachedExtentRef>>(),
      [this, extents=std::move(extents), &t](auto& alloc_map) {
      for (auto& extent : extents) {
        auto writer = &(get_writer(extent->hint));
        alloc_map[writer].emplace_back(extent);
      }
      return trans_intr::do_for_each(alloc_map, [&t](auto& p) {
        auto writer = p.first;
        auto& extents_to_pesist = p.second;
        return writer->write(t, extents_to_pesist);
      });
    });
  }

  stop_ertr::future<> stop() {
    return crimson::do_for_each(writers, [](auto& writer) {
      return writer.stop();
    });
  }
private:
  SegmentProvider& segment_provider;
  SegmentManager& segment_manager;
  std::vector<Writer> writers;
  LBAManager& lba_manager;
  Journal& journal;
  Cache& cache;
};

class ExtentPlacementManager {
public:
  ExtentPlacementManager(
    Cache& cache,
    LBAManager& lba_manager
  ) : cache(cache), lba_manager(lba_manager) {}

  /**
   * alloc_new_extent_by_type
   *
   * Create a new extent, CachedExtent::poffset may not be set
   * if a delayed allocation is needed.
   */
  CachedExtentRef alloc_new_extent_by_type(
    Transaction& t,
    extent_types_t type,
    segment_off_t length,
    ool_placement_hint_t hint = ool_placement_hint_t::NONE) {
    // only logical extents should fall in this path
    assert(is_logical_type(type));
    auto dtype = get_allocator_type(hint);
    CachedExtentRef extent;
    // for extents that would be stored in NVDIMM/PMEM, no delayed
    // allocation is needed
    if (need_delayed_allocation(dtype)) {
      // set a unique temperary paddr, this is necessary because
      // transaction's write_set is indexed by paddr
      extent = cache.alloc_new_extent_by_type(t, type, length, true);
    } else {
      extent = cache.alloc_new_extent_by_type(t, type, length);
    }
    extent->backend_type = dtype;
    extent->hint = hint;
    return extent;
  }

  template<
    typename T,
    std::enable_if_t<std::is_base_of_v<LogicalCachedExtent, T>, int> = 0>
  TCachedExtentRef<T> alloc_new_extent(
    Transaction& t,
    segment_off_t length,
    ool_placement_hint_t hint = ool_placement_hint_t::NONE)
  {
    auto dtype = get_allocator_type(hint);
    TCachedExtentRef<T> extent;
    if (need_delayed_allocation(dtype)) {
      // set a unique temperary paddr, this is necessary because
      // transaction's write_set is indexed by paddr
      extent = cache.alloc_new_extent<T>(t, length, true);
    } else {
      extent = cache.alloc_new_extent<T>(t, length);
    }
    extent->backend_type = dtype;
    extent->hint = hint;
    return extent;
  }

  /**
   * delayed_alloc_or_ool_write
   *
   * Performs any outstanding ool writes and updates pending lba updates
   * accordingly
   */
  using alloc_paddr_iertr = ExtentOolWriter::write_iertr;
  alloc_paddr_iertr::future<> delayed_alloc_or_ool_write(
    Transaction& t) {
    LOG_PREFIX(ExtentPlacementManager::delayed_alloc_or_ool_write);
    DEBUGT("start", t);
    return seastar::do_with(
      std::map<ExtentAllocator*, std::list<LogicalCachedExtentRef>>(),
      std::list<std::pair<paddr_t, LogicalCachedExtentRef>>(),
      [this, &t](auto& alloc_map, auto& inline_list) mutable {
      LOG_PREFIX(ExtentPlacementManager::delayed_alloc_or_ool_write);
      auto& alloc_list = t.get_delayed_alloc_list();
      uint64_t num_ool_extents = 0;
      for (auto& extent : alloc_list) {
        // extents may be invalidated
        if (!extent->is_valid()) {
          t.increment_delayed_invalid_extents();
          continue;
        }
        if (should_be_inline(extent)) {
          auto old_addr = extent->get_paddr();
          cache.mark_delayed_extent_inline(t, extent);
          inline_list.emplace_back(old_addr, extent);
        } else {
	  auto& allocator_ptr = get_allocator(
	    extent->backend_type, extent->hint
	  );
	  alloc_map[allocator_ptr.get()].emplace_back(extent);
	  num_ool_extents++;
	}
      }
      DEBUGT("{} inline extents, {} ool extents",
        t,
        inline_list.size(),
        num_ool_extents);
      return trans_intr::do_for_each(alloc_map, [&t](auto& p) {
        auto allocator = p.first;
        auto& extents = p.second;
        return allocator->alloc_ool_extents_paddr(t, extents);
      }).si_then([&inline_list, this, &t] {
        LOG_PREFIX(ExtentPlacementManager::delayed_alloc_or_ool_write);
        DEBUGT("processing {} inline extents", t, inline_list.size());
        return trans_intr::do_for_each(inline_list, [this, &t](auto& p) {
          auto old_addr = p.first;
          auto& extent = p.second;
          return lba_manager.update_mapping(
            t,
            extent->get_laddr(),
            old_addr,
            extent->get_paddr());
        });
      });
    });
  }

  void add_allocator(device_type_t type, ExtentAllocatorRef&& allocator) {
    allocators[type].emplace_back(std::move(allocator));
  }

private:
  device_type_t get_allocator_type(ool_placement_hint_t hint) {
    return device_type_t::SEGMENTED;
  }

  bool should_be_inline(LogicalCachedExtentRef& extent) {
    return (std::rand() % 2) == 0;
  }

  ExtentAllocatorRef& get_allocator(
    device_type_t type,
    ool_placement_hint_t hint) {
    auto& devices = allocators[type];
    return devices[std::rand() % devices.size()];
  }

  Cache& cache;
  LBAManager& lba_manager;
  std::map<device_type_t, std::vector<ExtentAllocatorRef>> allocators;
};
using ExtentPlacementManagerRef = std::unique_ptr<ExtentPlacementManager>;

}
