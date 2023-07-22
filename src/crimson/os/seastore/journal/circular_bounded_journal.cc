// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <boost/iterator/counting_iterator.hpp>

#include "crimson/common/errorator-loop.h"
#include "include/intarith.h"
#include "crimson/os/seastore/async_cleaner.h"
#include "crimson/os/seastore/journal/circular_bounded_journal.h"
#include "crimson/os/seastore/logging.h"
#include "crimson/os/seastore/journal/circular_journal_space.h"

SET_SUBSYS(seastore_journal);

namespace crimson::os::seastore::journal {

CircularBoundedJournal::CircularBoundedJournal(
    JournalTrimmer &trimmer,
    RBMDevice* device,
    const std::string &path)
  : trimmer(trimmer), path(path),
  cjs(device),
  record_submitter(crimson::common::get_conf<uint64_t>(
      "seastore_journal_iodepth_limit"),
    crimson::common::get_conf<uint64_t>(
      "seastore_journal_batch_capacity"),
    crimson::common::get_conf<Option::size_t>(
      "seastore_journal_batch_flush_size"),
    crimson::common::get_conf<double>(
      "seastore_journal_batch_preferred_fullness"),
    cjs)
  {}

CircularBoundedJournal::open_for_mkfs_ret
CircularBoundedJournal::open_for_mkfs()
{
  return record_submitter.open(true
  ).safe_then([this](auto ret) {
    record_submitter.update_committed_to(get_written_to());
    return open_for_mkfs_ret(
      open_for_mkfs_ertr::ready_future_marker{},
      get_written_to());
  });
}

CircularBoundedJournal::open_for_mount_ret
CircularBoundedJournal::open_for_mount()
{
  return record_submitter.open(false
  ).safe_then([this](auto ret) {
    record_submitter.update_committed_to(get_written_to());
    return open_for_mount_ret(
      open_for_mount_ertr::ready_future_marker{},
      get_written_to());
  });
}

CircularBoundedJournal::close_ertr::future<> CircularBoundedJournal::close()
{
  return record_submitter.close();
}

CircularBoundedJournal::submit_record_ret
CircularBoundedJournal::submit_record(
    record_t &&record,
    OrderingHandle &handle)
{
  LOG_PREFIX(CircularBoundedJournal::submit_record);
  DEBUG("H{} {} start ...", (void*)&handle, record);
  assert(write_pipeline);
  return do_submit_record(std::move(record), handle);
}

CircularBoundedJournal::submit_record_ret
CircularBoundedJournal::do_submit_record(
  record_t &&record,
  OrderingHandle &handle)
{
  LOG_PREFIX(CircularBoundedJournal::do_submit_record);
  if (!record_submitter.is_available()) {
    DEBUG("H{} wait ...", (void*)&handle);
    return record_submitter.wait_available(
    ).safe_then([this, record=std::move(record), &handle]() mutable {
      return do_submit_record(std::move(record), handle);
    });
  }
  auto action = record_submitter.check_action(record.size);
  if (action == RecordSubmitter::action_t::ROLL) {
    return record_submitter.roll_segment(
    ).safe_then([this, record=std::move(record), &handle]() mutable {
      return do_submit_record(std::move(record), handle);
    });
  }

  DEBUG("H{} submit {} ...",
	(void*)&handle,
	action == RecordSubmitter::action_t::SUBMIT_FULL ?
	"FULL" : "NOT_FULL");
  auto submit_fut = record_submitter.submit(std::move(record));
  return handle.enter(write_pipeline->device_submission
  ).then([submit_fut=std::move(submit_fut)]() mutable {
    return std::move(submit_fut);
  }).safe_then([FNAME, this, &handle](record_locator_t result) {
    return handle.enter(write_pipeline->finalize
    ).then([FNAME, this, result, &handle] {
      DEBUG("H{} finish with {}", (void*)&handle, result);
      auto new_committed_to = result.write_result.get_end_seq();
      record_submitter.update_committed_to(new_committed_to);
      return result;
    });
  });
}

RecordScanner::read_validate_record_metadata_ret CircularBoundedJournal::read_validate_record_metadata(
  scan_valid_records_cursor &cursor,
  segment_nonce_t nonce)
{
  LOG_PREFIX(CircularBoundedJournal::read_validate_record_metadata);
  paddr_t start = cursor.seq.offset;
  return read_record(start, nonce
  ).safe_then([FNAME, &cursor](auto ret) {
    if (!ret.has_value()) {
      return read_validate_record_metadata_ret(
	read_validate_record_metadata_ertr::ready_future_marker{},
	std::nullopt);
    }
    auto [r_header, bl] = *ret;
    if ((cursor.last_committed != JOURNAL_SEQ_NULL &&
	cursor.last_committed > r_header.committed_to) ||
	r_header.committed_to.segment_seq != cursor.seq.segment_seq) {
      DEBUG("invalid header: {}", r_header);
      return read_validate_record_metadata_ret(
	read_validate_record_metadata_ertr::ready_future_marker{},
	std::nullopt);
    }

    bufferlist mdbuf;
    mdbuf.substr_of(bl, 0, r_header.mdlength);
    DEBUG("header: {}", r_header);
    return read_validate_record_metadata_ret(
      read_validate_record_metadata_ertr::ready_future_marker{},
      std::make_pair(std::move(r_header), std::move(mdbuf)));
  });
}

RecordScanner::read_validate_data_ret CircularBoundedJournal::read_validate_data(
  paddr_t record_base,
  const record_group_header_t &header)
{
  return read_record(record_base, header.segment_nonce
  ).safe_then([](auto ret) {
    // read_record would return non-empty value if the record is valid
    if (!ret.has_value()) {
      return read_validate_data_ret(
        read_validate_data_ertr::ready_future_marker{},
        false);
    }
    return read_validate_data_ertr::make_ready_future<bool>(true);
  });
}

Journal::replay_ret CircularBoundedJournal::replay_segment(
   cbj_delta_handler_t &handler, scan_valid_records_cursor& cursor)
{
  LOG_PREFIX(Journal::replay_segment);
  return seastar::do_with(
    RecordScanner::found_record_handler_t(
      [this, &handler, FNAME](
      record_locator_t locator,
      const record_group_header_t& r_header,
      const bufferlist& mdbuf)
      -> RecordScanner::scan_valid_records_ertr::future<>
    {
      auto maybe_record_deltas_list = try_decode_deltas(
        r_header, mdbuf, locator.record_block_base);
      if (!maybe_record_deltas_list) {
        // This should be impossible, we did check the crc on the mdbuf
        ERROR("unable to decode deltas for record {} at {}",
              r_header, locator.record_block_base);
        return crimson::ct_error::input_output_error::make();
      }
      auto cursor_addr = convert_paddr_to_abs_addr(r_header.committed_to.offset);
      DEBUG("{} at {}", r_header, cursor_addr);
      auto write_result = write_result_t{
        r_header.committed_to,
        r_header.mdlength + r_header.dlength
      };
      auto expected_seq = r_header.committed_to.segment_seq;
      cursor_addr += (r_header.mdlength + r_header.dlength);
      if (cursor_addr >= get_journal_end()) {
        cursor_addr = get_records_start();
        ++expected_seq;
        paddr_t addr = convert_abs_addr_to_paddr(
          cursor_addr,
          get_device_id());
        write_result.start_seq.offset = addr;
        write_result.start_seq.segment_seq = expected_seq;
      }
      paddr_t addr = convert_abs_addr_to_paddr(
        cursor_addr,
        get_device_id());
      set_written_to(
        journal_seq_t{expected_seq, addr});
      return seastar::do_with(
        std::move(*maybe_record_deltas_list),
        [write_result,
        &handler,
        FNAME](auto& record_deltas_list) {
        return crimson::do_for_each(
          record_deltas_list,
          [write_result,
          &handler, FNAME](record_deltas_t& record_deltas) {
          auto locator = record_locator_t{
            record_deltas.record_block_base,
            write_result
          };
          DEBUG("processing {} deltas at block_base {}",
              record_deltas.deltas.size(),
              locator);
          return crimson::do_for_each(
            record_deltas.deltas,
            [locator,
            &handler](auto& p) {
            auto& modify_time = p.first;
            auto& delta = p.second;
            return handler(
              locator,
              delta,
              modify_time).discard_result();
          });
        });
      });
    }),
    [=, this, &cursor](auto &dhandler) {
      return scan_valid_records(
        cursor,
	cjs.get_cbj_header().magic,
        std::numeric_limits<size_t>::max(),
        dhandler).safe_then([](auto){}
      ).handle_error(
        replay_ertr::pass_further{},
        crimson::ct_error::assert_all{
          "shouldn't meet with any other error other replay_ertr"
        }
      );
    }
  );
}


Journal::replay_ret CircularBoundedJournal::scan_valid_record_delta(
   cbj_delta_handler_t &&handler, journal_seq_t tail)
{
  LOG_PREFIX(Journal::scan_valid_record_delta);
  INFO("starting at {} ", tail);
  return seastar::do_with(
    scan_valid_records_cursor(tail),
    std::move(handler),
    bool(false),
    [this] (auto &cursor, auto &handler, auto &rolled) {
    return crimson::repeat([this, &handler, &cursor, &rolled]()
    -> replay_ertr::future<seastar::stop_iteration>
    {
      return replay_segment(handler, cursor
      ).safe_then([this, &cursor, &rolled] {
        if (!rolled) {
          cursor.last_valid_header_found = false;
        }
        if (!cursor.is_complete()) {
          try_read_rolled_header(cursor);
	  rolled = true;
          return replay_ertr::make_ready_future<
            seastar::stop_iteration>(seastar::stop_iteration::no);
        }
        return replay_ertr::make_ready_future<
          seastar::stop_iteration>(seastar::stop_iteration::yes);
      });
    });
  });
}


Journal::replay_ret CircularBoundedJournal::replay(
    delta_handler_t &&delta_handler)
{
  /*
   * read records from last applied record prior to written_to, and replay
   */
  LOG_PREFIX(CircularBoundedJournal::replay);
  return cjs.read_header(
  ).handle_error(
    open_for_mount_ertr::pass_further{},
    crimson::ct_error::assert_all{
      "Invalid error read_header"
  }).safe_then([this, FNAME, delta_handler=std::move(delta_handler)](auto p)
    mutable {
    auto &[head, bl] = *p;
    cjs.set_cbj_header(head);
    DEBUG("header : {}", cjs.get_cbj_header());
    cjs.set_initialized(true);
    return seastar::do_with(
      std::move(delta_handler),
      std::map<paddr_t, journal_seq_t>(),
      [this](auto &d_handler, auto &map) {
      auto build_paddr_seq_map = [&map](
        const auto &offsets,
        const auto &e,
	sea_time_point modify_time)
      {
	if (e.type == extent_types_t::ALLOC_INFO) {
	  alloc_delta_t alloc_delta;
	  decode(alloc_delta, e.bl);
	  if (alloc_delta.op == alloc_delta_t::op_types_t::CLEAR) {
	    for (auto &alloc_blk : alloc_delta.alloc_blk_ranges) {
	      map[alloc_blk.paddr] = offsets.write_result.start_seq;
	    }
	  }
	}
	return replay_ertr::make_ready_future<bool>(true);
      };
      auto tail = get_dirty_tail() <= get_alloc_tail() ?
	get_dirty_tail() : get_alloc_tail();
      set_written_to(tail);
      // The first pass to build the paddr->journal_seq_t map 
      // from extent allocations
      return scan_valid_record_delta(std::move(build_paddr_seq_map), tail
      ).safe_then([this, &map, &d_handler, tail]() {
	auto call_d_handler_if_valid = [this, &map, &d_handler](
	  const auto &offsets,
	  const auto &e,
	  sea_time_point modify_time)
	{
	  if (map.find(e.paddr) == map.end() ||
	      map[e.paddr] <= offsets.write_result.start_seq) {
	    return d_handler(
	      offsets,
	      e,
	      get_dirty_tail(),
	      get_alloc_tail(),
	      modify_time
	    );
	  }
	  return replay_ertr::make_ready_future<bool>(true);
	};
	// The second pass to replay deltas
	return scan_valid_record_delta(std::move(call_d_handler_if_valid), tail);
      });
    }).safe_then([this]() {
      record_submitter.update_committed_to(get_written_to());
      trimmer.update_journal_tails(
	get_dirty_tail(),
	get_alloc_tail());
    });
  });
}

CircularBoundedJournal::read_record_ret
CircularBoundedJournal::return_record(record_group_header_t& header, bufferlist bl)
{
  LOG_PREFIX(CircularBoundedJournal::return_record);
  DEBUG("record size {}", bl.length());
  assert(bl.length() == header.mdlength + header.dlength);
  bufferlist md_bl, data_bl;
  md_bl.substr_of(bl, 0, header.mdlength);
  data_bl.substr_of(bl, header.mdlength, header.dlength);
  if (validate_records_metadata(md_bl) &&
      validate_records_data(header, data_bl)) {
    return read_record_ret(
      read_record_ertr::ready_future_marker{},
      std::make_pair(header, std::move(bl)));
  } else {
    DEBUG("invalid matadata");
    return read_record_ret(
      read_record_ertr::ready_future_marker{},
      std::nullopt);
  }
}

CircularBoundedJournal::read_record_ret
CircularBoundedJournal::read_record(paddr_t off, segment_nonce_t magic)
{
  LOG_PREFIX(CircularBoundedJournal::read_record);
  rbm_abs_addr addr = convert_paddr_to_abs_addr(off);
  auto read_length = get_block_size();
  assert(addr + read_length <= get_journal_end());
  DEBUG("reading record from abs addr {} read length {}", addr, read_length);
  auto bptr = bufferptr(ceph::buffer::create_page_aligned(read_length));
  return cjs.read(addr, bptr
  ).safe_then([this, addr, bptr, magic, FNAME]() mutable
    -> read_record_ret {
    record_group_header_t h;
    bufferlist bl;
    bl.append(bptr);
    auto bp = bl.cbegin();
    try {
      decode(h, bp);
    } catch (ceph::buffer::error &e) {
      return read_record_ret(
	read_record_ertr::ready_future_marker{},
	std::nullopt);
    }
    if (h.mdlength < get_block_size() ||
        h.mdlength % get_block_size() != 0 ||
        h.dlength % get_block_size() != 0 ||
        addr + h.mdlength + h.dlength > get_journal_end() ||
        h.committed_to.segment_seq == NULL_SEG_SEQ ||
	h.segment_nonce != magic) {
      return read_record_ret(
        read_record_ertr::ready_future_marker{},
        std::nullopt);
    }
    auto record_size = h.mdlength + h.dlength;
    if (record_size > get_block_size()) {
      auto next_addr = addr + get_block_size();
      auto next_length = record_size - get_block_size();
      auto next_bptr = bufferptr(ceph::buffer::create_page_aligned(next_length));
      DEBUG("reading record part 2 from abs addr {} read length {}",
            next_addr, next_length);
      return cjs.read(next_addr, next_bptr
      ).safe_then([this, h, next_bptr=std::move(next_bptr), bl=std::move(bl)]() mutable {
        bl.append(next_bptr);
        return return_record(h, bl);
      });
    } else {
      assert(record_size == get_block_size());
      return return_record(h, bl);
    }
  });
}

seastar::future<> CircularBoundedJournal::finish_commit(transaction_type_t type) {
  if (is_trim_transaction(type)) {
    return update_journal_tail(
      trimmer.get_dirty_tail(),
      trimmer.get_alloc_tail());
  }
  return seastar::now();
}

}
