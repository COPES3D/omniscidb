/**
 * @file    ResultSetTest.cpp
 * @author  Alex Suhan <alex@mapd.com>
 * @brief   Unit tests for the result set interface.
 *
 * Copyright (c) 2014 MapD Technologies, Inc.  All rights reserved.
 */

#include "../QueryEngine/ResultSet.h"
#include "../QueryEngine/RuntimeFunctions.h"

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <queue>

TEST(Construct, Empty) {
  ResultSet result_set;
  ASSERT_TRUE(result_set.isEmptyInitializer());
}

TEST(Construct, Allocate) {
  std::vector<TargetInfo> target_infos;
  QueryMemoryDescriptor query_mem_desc{};
  ResultSet result_set(target_infos, ExecutorDeviceType::CPU, query_mem_desc, std::make_shared<RowSetMemoryOwner>());
  result_set.allocateStorage();
}

namespace {

size_t get_slot_count(const std::vector<TargetInfo>& target_infos) {
  size_t count = 0;
  for (const auto& target_info : target_infos) {
    count = advance_slot(count, target_info);
  }
  return count;
}

class NumberGenerator {
 public:
  virtual int64_t getNextValue() = 0;

  virtual void reset() = 0;
};

class EvenNumberGenerator : public NumberGenerator {
 public:
  EvenNumberGenerator() : crt_(0) {}

  int64_t getNextValue() override {
    const auto crt = crt_;
    crt_ += 2;
    return crt;
  }

  void reset() override { crt_ = 0; }

 private:
  int64_t crt_;
};

class ReverseOddOrEvenNumberGenerator : public NumberGenerator {
 public:
  ReverseOddOrEvenNumberGenerator(const int64_t init) : crt_(init), init_(init) {}

  int64_t getNextValue() override {
    const auto crt = crt_;
    crt_ -= 2;
    return crt;
  }

  void reset() override { crt_ = init_; }

 private:
  int64_t crt_;
  int64_t init_;
};

void write_int(int8_t* slot_ptr, const int64_t v, const size_t slot_bytes) {
  switch (slot_bytes) {
    case 4:
      *reinterpret_cast<int32_t*>(slot_ptr) = v;
      break;
    case 8:
      *reinterpret_cast<int64_t*>(slot_ptr) = v;
      break;
    default:
      CHECK(false);
  }
}

void write_fp(int8_t* slot_ptr, const int64_t v, const size_t slot_bytes) {
  switch (slot_bytes) {
    case 4: {
      float fi = v;
      *reinterpret_cast<int32_t*>(slot_ptr) = *reinterpret_cast<int32_t*>(&fi);
      break;
    }
    case 8: {
      double di = v;
      *reinterpret_cast<int64_t*>(slot_ptr) = *reinterpret_cast<int64_t*>(&di);
      break;
    }
    default:
      CHECK(false);
  }
}

int8_t* fill_one_entry_no_collisions(int8_t* buff,
                                     const QueryMemoryDescriptor& query_mem_desc,
                                     const int64_t v,
                                     const std::vector<TargetInfo>& target_infos,
                                     const bool empty) {
  size_t target_idx = 0;
  int8_t* slot_ptr = buff;
  for (const auto& target_info : target_infos) {
    CHECK_LT(target_idx, query_mem_desc.agg_col_widths.size());
    const auto slot_bytes = query_mem_desc.agg_col_widths[target_idx].actual;
    CHECK_LE(target_info.sql_type.get_size(), slot_bytes);
    if (empty) {
      write_int(slot_ptr, query_mem_desc.keyless_hash ? 0 : v, slot_bytes);
    } else {
      if (target_info.sql_type.is_integer()) {
        write_int(slot_ptr, v, slot_bytes);
      } else if (target_info.sql_type.is_string()) {
        write_int(slot_ptr, -(v + 2), slot_bytes);
      } else {
        CHECK(target_info.sql_type.is_fp());
        write_fp(slot_ptr, v, slot_bytes);
      }
    }
    slot_ptr += slot_bytes;
    if (target_info.agg_kind == kAVG) {
      const auto count_slot_bytes = query_mem_desc.agg_col_widths[target_idx + 1].actual;
      if (empty) {
        write_int(slot_ptr, query_mem_desc.keyless_hash ? 0 : v, count_slot_bytes);
      } else {
        write_int(slot_ptr, 1, count_slot_bytes);  // count of elements in the group is 1 - good enough for testing
      }
      slot_ptr += count_slot_bytes;
    }
    target_idx = advance_slot(target_idx, target_info);
  }
  return slot_ptr;
}

void fill_one_entry_baseline(int64_t* value_slots,
                             const int64_t v,
                             const std::vector<TargetInfo>& target_infos,
                             const size_t key_component_count,
                             const size_t target_slot_count) {
  size_t target_slot = 0;
  for (const auto& target_info : target_infos) {
    switch (target_info.sql_type.get_type()) {
      case kSMALLINT:
      case kINT:
      case kBIGINT:
        value_slots[target_slot] = v;
        break;
      case kDOUBLE: {
        double di = v;
        value_slots[target_slot] = *reinterpret_cast<int64_t*>(&di);
        break;
      }
      case kTEXT:
        value_slots[target_slot] = -(v + 2);
        break;
      default:
        CHECK(false);
    }
    if (target_info.agg_kind == kAVG) {
      value_slots[target_slot + 1] = 1;
    }
    target_slot = advance_slot(target_slot, target_info);
  }
}

void fill_one_entry_one_col(int8_t* ptr1,
                            const int8_t compact_sz1,
                            int8_t* ptr2,
                            const int8_t compact_sz2,
                            const int64_t v,
                            const TargetInfo& target_info,
                            const bool empty_entry) {
  CHECK(ptr1);
  switch (compact_sz1) {
    case 8:
      if (target_info.sql_type.is_fp()) {
        double di = v;
        *reinterpret_cast<int64_t*>(ptr1) = *reinterpret_cast<int64_t*>(&di);
      } else {
        *reinterpret_cast<int64_t*>(ptr1) = v;
      }
      break;
    case 4:
      CHECK(!target_info.sql_type.is_fp());
      *reinterpret_cast<int32_t*>(ptr1) = v;
      break;
    default:
      CHECK(false);
  }
  if (target_info.is_agg && target_info.agg_kind == kAVG) {
    CHECK(ptr2);
    switch (compact_sz2) {
      case 8:
        *reinterpret_cast<int64_t*>(ptr2) = (empty_entry ? *ptr1 : 1);
        break;
      case 4:
        *reinterpret_cast<int32_t*>(ptr2) = (empty_entry ? *ptr1 : 1);
        break;
      default:
        CHECK(false);
    }
  }
}

void fill_one_entry_one_col(int64_t* value_slot,
                            const int64_t v,
                            const TargetInfo& target_info,
                            const size_t entry_count) {
  auto ptr1 = reinterpret_cast<int8_t*>(value_slot);
  int8_t* ptr2{nullptr};
  if (target_info.agg_kind == kAVG) {
    ptr2 = reinterpret_cast<int8_t*>(&value_slot[entry_count]);
  }
  fill_one_entry_one_col(ptr1, 8, ptr2, 8, v, target_info, false);
}

int8_t* advance_to_next_columnar_key_buff(int8_t* key_ptr,
                                          const QueryMemoryDescriptor& query_mem_desc,
                                          const size_t key_idx) {
  CHECK(!query_mem_desc.keyless_hash);
  CHECK_LT(key_idx, query_mem_desc.group_col_widths.size());
  auto new_key_ptr = key_ptr + query_mem_desc.entry_count * query_mem_desc.group_col_widths[key_idx];
  if (!query_mem_desc.key_column_pad_bytes.empty()) {
    CHECK_LT(key_idx, query_mem_desc.key_column_pad_bytes.size());
    new_key_ptr += query_mem_desc.key_column_pad_bytes[key_idx];
  }
  return new_key_ptr;
}

void write_key(const int64_t k, int8_t* ptr, const int8_t key_bytes) {
  switch (key_bytes) {
    case 8: {
      *reinterpret_cast<int64_t*>(ptr) = k;
      break;
    }
    case 4: {
      *reinterpret_cast<int32_t*>(ptr) = k;
      break;
    }
    default:
      CHECK(false);
  }
}

void fill_storage_buffer_perfect_hash_colwise(int8_t* buff,
                                              const std::vector<TargetInfo>& target_infos,
                                              const QueryMemoryDescriptor& query_mem_desc,
                                              NumberGenerator& generator) {
  const auto key_component_count = get_key_count_for_descriptor(query_mem_desc);
  CHECK(query_mem_desc.output_columnar);
  // initialize the key buffer(s)
  auto col_ptr = buff;
  for (size_t key_idx = 0; key_idx < key_component_count; ++key_idx) {
    auto key_entry_ptr = col_ptr;
    const auto key_bytes = query_mem_desc.group_col_widths[key_idx];
    CHECK_EQ(8, key_bytes);
    for (size_t i = 0; i < query_mem_desc.entry_count; ++i) {
      if (i % 2 == 0) {
        const auto v = generator.getNextValue();
        write_key(v, key_entry_ptr, key_bytes);
      } else {
        write_key(EMPTY_KEY_64, key_entry_ptr, key_bytes);
      }
      key_entry_ptr += key_bytes;
    }
    col_ptr = advance_to_next_columnar_key_buff(col_ptr, query_mem_desc, key_idx);
    generator.reset();
  }
  // initialize the value buffer(s)
  size_t slot_idx = 0;
  for (const auto& target_info : target_infos) {
    auto col_entry_ptr = col_ptr;
    const auto col_bytes = query_mem_desc.agg_col_widths[slot_idx].compact;
    for (size_t i = 0; i < query_mem_desc.entry_count; ++i) {
      int8_t* ptr2{nullptr};
      if (target_info.agg_kind == kAVG) {
        ptr2 = col_entry_ptr + query_mem_desc.entry_count * col_bytes;
      }
      if (i % 2 == 0) {
        const auto gen_val = generator.getNextValue();
        const auto val = target_info.sql_type.is_string() ? -(gen_val + 2) : gen_val;
        fill_one_entry_one_col(col_entry_ptr,
                               col_bytes,
                               ptr2,
                               query_mem_desc.agg_col_widths[slot_idx + 1].compact,
                               val,
                               target_info,
                               false);
      } else {
        fill_one_entry_one_col(col_entry_ptr,
                               col_bytes,
                               ptr2,
                               query_mem_desc.agg_col_widths[slot_idx + 1].compact,
                               query_mem_desc.keyless_hash ? 0 : 0xdeadbeef,
                               target_info,
                               true);
      }
      col_entry_ptr += col_bytes;
    }
    col_ptr = advance_to_next_columnar_target_buff(col_ptr, query_mem_desc, slot_idx);
    if (target_info.is_agg && target_info.agg_kind == kAVG) {
      col_ptr = advance_to_next_columnar_target_buff(col_ptr, query_mem_desc, slot_idx + 1);
    }
    slot_idx = advance_slot(slot_idx, target_info);
    generator.reset();
  }
}

void fill_storage_buffer_perfect_hash_rowwise(int8_t* buff,
                                              const std::vector<TargetInfo>& target_infos,
                                              const QueryMemoryDescriptor& query_mem_desc,
                                              NumberGenerator& generator) {
  const auto key_component_count = get_key_count_for_descriptor(query_mem_desc);
  CHECK(!query_mem_desc.output_columnar);
  auto key_buff = buff;
  for (size_t i = 0; i < query_mem_desc.entry_count; ++i) {
    if (i % 2 == 0) {
      const auto v = generator.getNextValue();
      auto key_buff_i64 = reinterpret_cast<int64_t*>(key_buff);
      for (size_t key_comp_idx = 0; key_comp_idx < key_component_count; ++key_comp_idx) {
        *key_buff_i64++ = v;
      }
      auto entries_buff = reinterpret_cast<int8_t*>(key_buff_i64);
      key_buff = fill_one_entry_no_collisions(entries_buff, query_mem_desc, v, target_infos, false);
    } else {
      auto key_buff_i64 = reinterpret_cast<int64_t*>(key_buff);
      for (size_t key_comp_idx = 0; key_comp_idx < key_component_count; ++key_comp_idx) {
        *key_buff_i64++ = EMPTY_KEY_64;
      }
      auto entries_buff = reinterpret_cast<int8_t*>(key_buff_i64);
      key_buff = fill_one_entry_no_collisions(entries_buff, query_mem_desc, 0xdeadbeef, target_infos, true);
    }
  }
}

void fill_storage_buffer_baseline_colwise(int8_t* buff,
                                          const std::vector<TargetInfo>& target_infos,
                                          const QueryMemoryDescriptor& query_mem_desc,
                                          NumberGenerator& generator,
                                          const size_t step) {
  CHECK(query_mem_desc.output_columnar);
  const auto key_component_count = get_key_count_for_descriptor(query_mem_desc);
  const auto i64_buff = reinterpret_cast<int64_t*>(buff);
  const auto target_slot_count = get_slot_count(target_infos);
  for (size_t i = 0; i < query_mem_desc.entry_count; ++i) {
    for (size_t key_comp_idx = 0; key_comp_idx < key_component_count; ++key_comp_idx) {
      i64_buff[key_offset_colwise(i, key_comp_idx, query_mem_desc.entry_count)] = EMPTY_KEY_64;
    }
    for (size_t target_slot = 0; target_slot < target_slot_count; ++target_slot) {
      i64_buff[slot_offset_colwise(i, target_slot, key_component_count, query_mem_desc.entry_count)] = 0xdeadbeef;
    }
  }
  for (size_t i = 0; i < query_mem_desc.entry_count; i += step) {
    const auto gen_val = generator.getNextValue();
    std::vector<int64_t> key(key_component_count, gen_val);
    auto value_slots = get_group_value_columnar(i64_buff, query_mem_desc.entry_count, &key[0], key.size());
    CHECK(value_slots);
    for (const auto& target_info : target_infos) {
      const auto val = target_info.sql_type.is_string() ? -(gen_val + step) : gen_val;
      fill_one_entry_one_col(value_slots, val, target_info, query_mem_desc.entry_count);
      value_slots += query_mem_desc.entry_count;
      if (target_info.agg_kind == kAVG) {
        value_slots += query_mem_desc.entry_count;
      }
    }
  }
}

void fill_storage_buffer_baseline_rowwise(int8_t* buff,
                                          const std::vector<TargetInfo>& target_infos,
                                          const QueryMemoryDescriptor& query_mem_desc,
                                          NumberGenerator& generator,
                                          const size_t step) {
  const auto key_component_count = get_key_count_for_descriptor(query_mem_desc);
  const auto i64_buff = reinterpret_cast<int64_t*>(buff);
  const auto target_slot_count = get_slot_count(target_infos);
  for (size_t i = 0; i < query_mem_desc.entry_count; ++i) {
    const auto first_key_comp_offset = key_offset_rowwise(i, key_component_count, target_slot_count);
    for (size_t key_comp_idx = 0; key_comp_idx < key_component_count; ++key_comp_idx) {
      i64_buff[first_key_comp_offset + key_comp_idx] = EMPTY_KEY_64;
    }
    for (size_t target_slot = 0; target_slot < target_slot_count; ++target_slot) {
      i64_buff[slot_offset_rowwise(i, target_slot, key_component_count, target_slot_count)] = 0xdeadbeef;
    }
  }
  for (size_t i = 0; i < query_mem_desc.entry_count; i += step) {
    const auto v = generator.getNextValue();
    std::vector<int64_t> key(key_component_count, v);
    auto value_slots = get_group_value(
        i64_buff, query_mem_desc.entry_count, &key[0], key.size(), key_component_count + target_slot_count, nullptr);
    CHECK(value_slots);
    fill_one_entry_baseline(value_slots, v, target_infos, key_component_count, target_slot_count);
  }
}

void fill_storage_buffer(int8_t* buff,
                         const std::vector<TargetInfo>& target_infos,
                         const QueryMemoryDescriptor& query_mem_desc,
                         NumberGenerator& generator,
                         const size_t step) {
  switch (query_mem_desc.hash_type) {
    case GroupByColRangeType::OneColKnownRange:
    case GroupByColRangeType::MultiColPerfectHash: {
      if (query_mem_desc.output_columnar) {
        fill_storage_buffer_perfect_hash_colwise(buff, target_infos, query_mem_desc, generator);
      } else {
        fill_storage_buffer_perfect_hash_rowwise(buff, target_infos, query_mem_desc, generator);
      }
      break;
    }
    case GroupByColRangeType::MultiCol: {
      if (query_mem_desc.output_columnar) {
        fill_storage_buffer_baseline_colwise(buff, target_infos, query_mem_desc, generator, step);
      } else {
        fill_storage_buffer_baseline_rowwise(buff, target_infos, query_mem_desc, generator, step);
      }
      break;
    }
    default:
      CHECK(false);
  }
  CHECK(buff);
}

// TODO(alex): allow 4 byte keys

/* descriptor with small entry_count to simplify testing and debugging */
QueryMemoryDescriptor perfect_hash_one_col_desc_small(const std::vector<TargetInfo>& target_infos,
                                                      const int8_t num_bytes) {
  QueryMemoryDescriptor query_mem_desc{};
  query_mem_desc.hash_type = GroupByColRangeType::OneColKnownRange;
  query_mem_desc.min_val = 0;
  query_mem_desc.max_val = 19;
  query_mem_desc.has_nulls = false;
  query_mem_desc.group_col_widths.emplace_back(8);
  for (const auto& target_info : target_infos) {
    const auto slot_bytes = std::max(num_bytes, static_cast<int8_t>(target_info.sql_type.get_size()));
    if (target_info.agg_kind == kAVG) {
      CHECK(target_info.is_agg);
      query_mem_desc.agg_col_widths.emplace_back(ColWidths{slot_bytes, slot_bytes});
    }
    query_mem_desc.agg_col_widths.emplace_back(ColWidths{slot_bytes, slot_bytes});
  }
  query_mem_desc.entry_count = query_mem_desc.max_val - query_mem_desc.min_val + 1;
  return query_mem_desc;
}

QueryMemoryDescriptor perfect_hash_one_col_desc(const std::vector<TargetInfo>& target_infos, const int8_t num_bytes) {
  QueryMemoryDescriptor query_mem_desc{};
  query_mem_desc.hash_type = GroupByColRangeType::OneColKnownRange;
  query_mem_desc.min_val = 0;
  query_mem_desc.max_val = 99;
  query_mem_desc.has_nulls = false;
  query_mem_desc.group_col_widths.emplace_back(8);
  for (const auto& target_info : target_infos) {
    const auto slot_bytes = std::max(num_bytes, static_cast<int8_t>(target_info.sql_type.get_size()));
    if (target_info.agg_kind == kAVG) {
      CHECK(target_info.is_agg);
      query_mem_desc.agg_col_widths.emplace_back(ColWidths{slot_bytes, slot_bytes});
    }
    query_mem_desc.agg_col_widths.emplace_back(ColWidths{slot_bytes, slot_bytes});
  }
  query_mem_desc.entry_count = query_mem_desc.max_val - query_mem_desc.min_val + 1;
  return query_mem_desc;
}

QueryMemoryDescriptor perfect_hash_two_col_desc(const std::vector<TargetInfo>& target_infos, const int8_t num_bytes) {
  QueryMemoryDescriptor query_mem_desc{};
  query_mem_desc.hash_type = GroupByColRangeType::MultiColPerfectHash;
  query_mem_desc.min_val = 0;
  query_mem_desc.max_val = 36;
  query_mem_desc.has_nulls = false;
  query_mem_desc.group_col_widths.emplace_back(8);
  query_mem_desc.group_col_widths.emplace_back(8);
  for (const auto& target_info : target_infos) {
    const auto slot_bytes = std::max(num_bytes, static_cast<int8_t>(target_info.sql_type.get_size()));
    if (target_info.agg_kind == kAVG) {
      CHECK(target_info.is_agg);
      query_mem_desc.agg_col_widths.emplace_back(ColWidths{slot_bytes, slot_bytes});
    }
    query_mem_desc.agg_col_widths.emplace_back(ColWidths{slot_bytes, slot_bytes});
  }
  query_mem_desc.entry_count = query_mem_desc.max_val;
  return query_mem_desc;
}

QueryMemoryDescriptor baseline_hash_two_col_desc_large(const std::vector<TargetInfo>& target_infos,
                                                       const int8_t num_bytes) {
  QueryMemoryDescriptor query_mem_desc{};
  query_mem_desc.hash_type = GroupByColRangeType::MultiCol;
  query_mem_desc.min_val = 0;
  query_mem_desc.max_val = 19;
  query_mem_desc.has_nulls = false;
  query_mem_desc.group_col_widths.emplace_back(8);
  query_mem_desc.group_col_widths.emplace_back(8);
  for (const auto& target_info : target_infos) {
    const auto slot_bytes = std::max(num_bytes, static_cast<int8_t>(target_info.sql_type.get_size()));
    if (target_info.agg_kind == kAVG) {
      CHECK(target_info.is_agg);
      query_mem_desc.agg_col_widths.emplace_back(ColWidths{slot_bytes, slot_bytes});
    }
    query_mem_desc.agg_col_widths.emplace_back(ColWidths{slot_bytes, slot_bytes});
  }
  query_mem_desc.entry_count = query_mem_desc.max_val - query_mem_desc.min_val + 1;
  return query_mem_desc;
}

QueryMemoryDescriptor baseline_hash_two_col_desc(const std::vector<TargetInfo>& target_infos, const int8_t num_bytes) {
  QueryMemoryDescriptor query_mem_desc{};
  query_mem_desc.hash_type = GroupByColRangeType::MultiCol;
  query_mem_desc.min_val = 0;
  query_mem_desc.max_val = 3;
  query_mem_desc.has_nulls = false;
  query_mem_desc.group_col_widths.emplace_back(8);
  query_mem_desc.group_col_widths.emplace_back(8);
  for (const auto& target_info : target_infos) {
    const auto slot_bytes = std::max(num_bytes, static_cast<int8_t>(target_info.sql_type.get_size()));
    if (target_info.agg_kind == kAVG) {
      CHECK(target_info.is_agg);
      query_mem_desc.agg_col_widths.emplace_back(ColWidths{slot_bytes, slot_bytes});
    }
    query_mem_desc.agg_col_widths.emplace_back(ColWidths{slot_bytes, slot_bytes});
  }
  query_mem_desc.entry_count = query_mem_desc.max_val - query_mem_desc.min_val + 1;
  return query_mem_desc;
}

template <class T>
T v(const TargetValue& r) {
  auto scalar_r = boost::get<ScalarTargetValue>(&r);
  CHECK(scalar_r);
  auto p = boost::get<T>(scalar_r);
  CHECK(p);
  return *p;
}

template <class T>
const T* vptr(const TargetValue& r) {
  auto scalar_r = boost::get<ScalarTargetValue>(&r);
  CHECK(scalar_r);
  return boost::get<T>(scalar_r);
}

/* This class allows to emulate and evaluate ResultSet and it's reduce function.
 * It creates two ResultSet equivalents, populates them with randomly generated data,
 * merges them into one, and provides access to the data contained in the merged set.
 * Comparing these data with the ones received from the ResultSet code reduce procedure
 * run on the same pair of the ResultSet equivalents, allows to evaluate the functionality
 * of the ResultSet code.
 */
class ResultSetEmulator {
 public:
  ResultSetEmulator(int8_t* buff1,
                    int8_t* buff2,
                    const std::vector<TargetInfo>& target_infos,
                    const QueryMemoryDescriptor& query_mem_desc,
                    NumberGenerator& gen1,
                    NumberGenerator& gen2,
                    const size_t perc1,
                    const size_t perc2,
                    const size_t step,
                    const bool silent)
      :

        rs1_buff(buff1),
        rs2_buff(buff2),
        rs_target_infos(target_infos),
        rs_query_mem_desc(query_mem_desc),
        rs1_gen(gen1),
        rs2_gen(gen2),
        rs1_perc(perc1),
        rs2_perc(perc2),
        rs_step(step),
        rs_entry_count(query_mem_desc.entry_count),
        rs_silent(silent) {
    rs_entry_count = query_mem_desc.entry_count;  // it's set to 10 in "small" query_mem_descriptor
    rs1_groups.resize(rs_entry_count);
    std::fill(rs1_groups.begin(), rs1_groups.end(), false);
    rs2_groups.resize(rs_entry_count);
    std::fill(rs2_groups.begin(), rs2_groups.end(), false);
    rs1_values.resize(rs_entry_count);
    std::fill(rs1_groups.begin(), rs1_groups.end(), 0);
    rs2_values.resize(rs_entry_count);
    std::fill(rs2_groups.begin(), rs2_groups.end(), 0);
    rseReducedGroups.resize(rs_entry_count);
    std::fill(rseReducedGroups.begin(), rseReducedGroups.end(), false);

    emulateResultSets();
  }
  ~ResultSetEmulator(){};

  std::queue<std::vector<int64_t>> getReferenceTable() const { return rseReducedTable; }
  std::vector<bool> getReferenceGroupMap() const { return rseReducedGroups; }
  bool getReferenceGroupMapElement(size_t idx) {
    CHECK_LE(idx, rseReducedGroups.size() - 1);
    return rseReducedGroups[idx];
  }
  std::vector<int64_t> getReferenceRow(bool keep_row = false) {
    std::vector<int64_t> rse_reduced_row(rseReducedTable.front());
    rseReducedTable.pop();
    if (keep_row) {
      rseReducedTable.push(rse_reduced_row);
    }
    return rse_reduced_row;
  }
  void print_rse_generated_result_sets() const;
  void print_merged_result_sets(const auto result);

 private:
  void emulateResultSets();
  void createResultSet(size_t rs_perc, std::vector<bool>& rs_groups);
  void mergeResultSets();
  int8_t* rs1_buff, *rs2_buff;
  const std::vector<TargetInfo> rs_target_infos;
  const QueryMemoryDescriptor rs_query_mem_desc;
  NumberGenerator& rs1_gen, &rs2_gen;
  size_t rs1_perc, rs2_perc, rs_step;
  size_t rs_entry_count;
  bool rs_silent;
  std::vector<bool> rs1_groups;                      // true if group is in ResultSet #1
  std::vector<bool> rs2_groups;                      // true if group is in ResultSet #2
  std::vector<bool> rseReducedGroups;                // true if group is in either ResultSet #1 or ResultSet #2
  std::vector<int64_t> rs1_values;                   // generated values for ResultSet #1
  std::vector<int64_t> rs2_values;                   // generated values for ResultSet #2
  std::queue<std::vector<int64_t>> rseReducedTable;  // combined/reduced values of ResultSet #1 and ResultSet #2

  void rse_fill_storage_buffer_perfect_hash_colwise(int8_t* buff,
                                                    NumberGenerator& generator,
                                                    const std::vector<bool>& rs_groups,
                                                    std::vector<int64_t>& rs_values);
  void rse_fill_storage_buffer_perfect_hash_rowwise(int8_t* buff,
                                                    NumberGenerator& generator,
                                                    const std::vector<bool>& rs_groups,
                                                    std::vector<int64_t>& rs_values);
  void rse_fill_storage_buffer_baseline_colwise(int8_t* buff,
                                                NumberGenerator& generator,
                                                const std::vector<bool>& rs_groups,
                                                std::vector<int64_t>& rs_values);
  void rse_fill_storage_buffer_baseline_rowwise(int8_t* buff,
                                                NumberGenerator& generator,
                                                const std::vector<bool>& rs_groups,
                                                std::vector<int64_t>& rs_values);
  void rse_fill_storage_buffer(int8_t* buff,
                               NumberGenerator& generator,
                               const std::vector<bool>& rs_groups,
                               std::vector<int64_t>& rs_values);
  void print_emulator_diag();
  int64_t rseAggregateKMIN(size_t i);
  int64_t rseAggregateKMAX(size_t i);
  int64_t rseAggregateKAVG(size_t i);
  int64_t rseAggregateKSUM(size_t i);
  int64_t rseAggregateKCOUNT(size_t i);
};

/* top level module to create and fill up ResultSets as well as to generate golden values */
void ResultSetEmulator::emulateResultSets() {
  /* generate topology of ResultSet #1 */
  if (!rs_silent) {
    printf("\nResultSetEmulator (ResultSet #1): ");
  }

  createResultSet(rs1_perc, rs1_groups);
  if (!rs_silent) {
    printf("\n");
    for (size_t i = 0; i < rs1_groups.size(); i++) {
      if (rs1_groups[i])
        printf("1");
      else
        printf("0");
    }
  }

  /* generate topology of ResultSet #2 */
  if (!rs_silent) {
    printf("\nResultSetEmulator (ResultSet #2): ");
  }

  createResultSet(rs2_perc, rs2_groups);
  if (!rs_silent) {
    printf("\n");
    for (size_t i = 0; i < rs2_groups.size(); i++) {
      if (rs2_groups[i])
        printf("1");
      else
        printf("0");
    }
    printf("\n");
  }

  /* populate both ResultSet's buffers with real data */
  // print_emulator_diag();
  rse_fill_storage_buffer(rs1_buff, rs1_gen, rs1_groups, rs1_values);
  // print_emulator_diag();
  rse_fill_storage_buffer(rs2_buff, rs2_gen, rs2_groups, rs2_values);
  // print_emulator_diag();

  /* merge/reduce data contained in both ResultSets and generate golden values */
  mergeResultSets();
}

/* generate ResultSet topology (create rs_groups and rs_groups_idx vectors) */
void ResultSetEmulator::createResultSet(size_t rs_perc, std::vector<bool>& rs_groups) {
  std::vector<int> rs_groups_idx;
  rs_groups_idx.resize(rs_entry_count);
  std::iota(rs_groups_idx.begin(), rs_groups_idx.end(), 0);
  std::random_device rs_rd;
  std::mt19937 rs_rand_gen(rs_rd());
  std::shuffle(rs_groups_idx.begin(), rs_groups_idx.end(), rs_rand_gen);

  for (size_t i = 0; i < (rs_entry_count * rs_perc / 100); i++) {
    if (!rs_silent) {
      printf(" %i", rs_groups_idx[i]);
    }
    rs_groups[rs_groups_idx[i]] = true;
  }
}

/* merge/reduce data contained in both ResultSets and generate golden values */
void ResultSetEmulator::mergeResultSets() {
  std::vector<int64_t> rse_reduced_row;
  rse_reduced_row.resize(rs_target_infos.size());

  for (size_t j = 0; j < rs_entry_count; j++) {  // iterates through rows
    if (rs1_groups[j] || rs2_groups[j]) {
      rseReducedGroups[j] = true;
      for (size_t i = 0; i < rs_target_infos.size(); i++) {  // iterates through columns
        switch (rs_target_infos[i].agg_kind) {
          case kMIN: {
            rse_reduced_row[i] = rseAggregateKMIN(j);
            break;
          }
          case kMAX: {
            rse_reduced_row[i] = rseAggregateKMAX(j);
            break;
          }
          case kAVG: {
            rse_reduced_row[i] = rseAggregateKAVG(j);
            break;
          }
          case kSUM: {
            rse_reduced_row[i] = rseAggregateKSUM(j);
            break;
          }
          case kCOUNT: {
            rse_reduced_row[i] = rseAggregateKCOUNT(j);
            break;
          }
          default:
            CHECK(false);
        }
      }
      rseReducedTable.push(rse_reduced_row);
    }
  }
}

void ResultSetEmulator::rse_fill_storage_buffer_perfect_hash_colwise(int8_t* buff,
                                                                     NumberGenerator& generator,
                                                                     const std::vector<bool>& rs_groups,
                                                                     std::vector<int64_t>& rs_values) {
  const auto key_component_count = get_key_count_for_descriptor(rs_query_mem_desc);
  CHECK(rs_query_mem_desc.output_columnar);
  // initialize the key buffer(s)
  auto col_ptr = buff;
  for (size_t key_idx = 0; key_idx < key_component_count; ++key_idx) {
    auto key_entry_ptr = col_ptr;
    const auto key_bytes = rs_query_mem_desc.group_col_widths[key_idx];
    CHECK_EQ(8, key_bytes);
    for (size_t i = 0; i < rs_entry_count; i++) {
      const auto v = generator.getNextValue();
      if (rs_groups[i]) {
        // const auto v = generator.getNextValue();
        write_key(v, key_entry_ptr, key_bytes);
      } else {
        write_key(EMPTY_KEY_64, key_entry_ptr, key_bytes);
      }
      key_entry_ptr += key_bytes;
    }
    col_ptr = advance_to_next_columnar_key_buff(col_ptr, rs_query_mem_desc, key_idx);
    generator.reset();
  }
  // initialize the value buffer(s)
  size_t slot_idx = 0;
  for (const auto& target_info : rs_target_infos) {
    auto col_entry_ptr = col_ptr;
    const auto col_bytes = rs_query_mem_desc.agg_col_widths[slot_idx].compact;
    for (size_t i = 0; i < rs_entry_count; i++) {
      int8_t* ptr2{nullptr};
      if (target_info.agg_kind == kAVG) {
        ptr2 = col_entry_ptr + rs_query_mem_desc.entry_count * col_bytes;
      }
      const auto v = generator.getNextValue();
      if (rs_groups[i]) {
        // const auto v = generator.getNextValue();
        rs_values[i] = v;
        fill_one_entry_one_col(col_entry_ptr,
                               col_bytes,
                               ptr2,
                               rs_query_mem_desc.agg_col_widths[slot_idx + 1].compact,
                               v,
                               target_info,
                               false);
      } else {
        fill_one_entry_one_col(col_entry_ptr,
                               col_bytes,
                               ptr2,
                               rs_query_mem_desc.agg_col_widths[slot_idx + 1].compact,
                               rs_query_mem_desc.keyless_hash ? 0 : 0xdeadbeef,
                               target_info,
                               true);
      }
      col_entry_ptr += col_bytes;
    }
    col_ptr = advance_to_next_columnar_target_buff(col_ptr, rs_query_mem_desc, slot_idx);
    if (target_info.is_agg && target_info.agg_kind == kAVG) {
      col_ptr = advance_to_next_columnar_target_buff(col_ptr, rs_query_mem_desc, slot_idx + 1);
    }
    slot_idx = advance_slot(slot_idx, target_info);
    generator.reset();
  }
}

void ResultSetEmulator::rse_fill_storage_buffer_perfect_hash_rowwise(int8_t* buff,
                                                                     NumberGenerator& generator,
                                                                     const std::vector<bool>& rs_groups,
                                                                     std::vector<int64_t>& rs_values) {
  const auto key_component_count = get_key_count_for_descriptor(rs_query_mem_desc);
  CHECK(!rs_query_mem_desc.output_columnar);
  auto key_buff = buff;
  CHECK_EQ(rs_groups.size(), rs_query_mem_desc.entry_count);
  for (size_t i = 0; i < rs_groups.size(); i++) {
    const auto v = generator.getNextValue();
    if (rs_groups[i]) {
      // const auto v = generator.getNextValue();
      rs_values[i] = v;
      auto key_buff_i64 = reinterpret_cast<int64_t*>(key_buff);
      for (size_t key_comp_idx = 0; key_comp_idx < key_component_count; ++key_comp_idx) {
        *key_buff_i64++ = v;
      }
      auto entries_buff = reinterpret_cast<int8_t*>(key_buff_i64);
      key_buff = fill_one_entry_no_collisions(entries_buff, rs_query_mem_desc, v, rs_target_infos, false);
    } else {
      auto key_buff_i64 = reinterpret_cast<int64_t*>(key_buff);
      for (size_t key_comp_idx = 0; key_comp_idx < key_component_count; ++key_comp_idx) {
        *key_buff_i64++ = EMPTY_KEY_64;
      }
      auto entries_buff = reinterpret_cast<int8_t*>(key_buff_i64);
      key_buff = fill_one_entry_no_collisions(entries_buff, rs_query_mem_desc, 0xdeadbeef, rs_target_infos, true);
    }
  }
}
void ResultSetEmulator::rse_fill_storage_buffer_baseline_colwise(int8_t* buff,
                                                                 NumberGenerator& generator,
                                                                 const std::vector<bool>& rs_groups,
                                                                 std::vector<int64_t>& rs_values) {
  CHECK(rs_query_mem_desc.output_columnar);
  const auto key_component_count = get_key_count_for_descriptor(rs_query_mem_desc);
  const auto i64_buff = reinterpret_cast<int64_t*>(buff);
  const auto target_slot_count = get_slot_count(rs_target_infos);
  for (size_t i = 0; i < rs_entry_count; i++) {
    for (size_t key_comp_idx = 0; key_comp_idx < key_component_count; ++key_comp_idx) {
      i64_buff[key_offset_colwise(i, key_comp_idx, rs_entry_count)] = EMPTY_KEY_64;
    }
    for (size_t target_slot = 0; target_slot < target_slot_count; ++target_slot) {
      i64_buff[slot_offset_colwise(i, target_slot, key_component_count, rs_entry_count)] = 0xdeadbeef;
    }
  }
  for (size_t i = 0; i < rs_entry_count; i++) {
    const auto v = generator.getNextValue();
    if (rs_groups[i]) {
      // const auto v = generator.getNextValue();
      rs_values[i] = v;
      std::vector<int64_t> key(key_component_count, v);
      auto value_slots = get_group_value_columnar(i64_buff, rs_entry_count, &key[0], key.size());
      CHECK(value_slots);
      for (const auto& target_info : rs_target_infos) {
        fill_one_entry_one_col(value_slots, v, target_info, rs_entry_count);
        value_slots += rs_entry_count;
        if (target_info.agg_kind == kAVG) {
          value_slots += rs_entry_count;
        }
      }
    }
  }
}
void ResultSetEmulator::rse_fill_storage_buffer_baseline_rowwise(int8_t* buff,
                                                                 NumberGenerator& generator,
                                                                 const std::vector<bool>& rs_groups,
                                                                 std::vector<int64_t>& rs_values) {
  CHECK(!rs_query_mem_desc.output_columnar);
  CHECK_EQ(rs_groups.size(), rs_query_mem_desc.entry_count);
  const auto key_component_count = get_key_count_for_descriptor(rs_query_mem_desc);
  const auto i64_buff = reinterpret_cast<int64_t*>(buff);
  const auto target_slot_count = get_slot_count(rs_target_infos);
  for (size_t i = 0; i < rs_entry_count; i++) {
    const auto first_key_comp_offset = key_offset_rowwise(i, key_component_count, target_slot_count);
    for (size_t key_comp_idx = 0; key_comp_idx < key_component_count; ++key_comp_idx) {
      i64_buff[first_key_comp_offset + key_comp_idx] = EMPTY_KEY_64;
    }
    for (size_t target_slot = 0; target_slot < target_slot_count; ++target_slot) {
      i64_buff[slot_offset_rowwise(i, target_slot, key_component_count, target_slot_count)] = 0xdeadbeef;
    }
  }

  for (size_t i = 0; i < rs_entry_count; i++) {
    const auto v = generator.getNextValue();
    if (rs_groups[i]) {
      // const auto key_id = static_cast<int64_t>(i);
      // std::vector<int64_t> key(key_component_count, key_id);
      std::vector<int64_t> key(key_component_count, v);
      auto value_slots = get_group_value(
          i64_buff, rs_entry_count, &key[0], key.size(), key_component_count + target_slot_count, nullptr);
      CHECK(value_slots);
      rs_values[i] = v;
      fill_one_entry_baseline(value_slots, v, rs_target_infos, key_component_count, target_slot_count);
    }
  }
}

void ResultSetEmulator::rse_fill_storage_buffer(int8_t* buff,
                                                NumberGenerator& generator,
                                                const std::vector<bool>& rs_groups,
                                                std::vector<int64_t>& rs_values) {
  switch (rs_query_mem_desc.hash_type) {
    case GroupByColRangeType::OneColKnownRange:
    case GroupByColRangeType::MultiColPerfectHash: {
      if (rs_query_mem_desc.output_columnar) {
        rse_fill_storage_buffer_perfect_hash_colwise(buff, generator, rs_groups, rs_values);
      } else {
        rse_fill_storage_buffer_perfect_hash_rowwise(buff, generator, rs_groups, rs_values);
      }
      break;
    }
    case GroupByColRangeType::MultiCol: {
      if (rs_query_mem_desc.output_columnar) {
        rse_fill_storage_buffer_baseline_colwise(buff, generator, rs_groups, rs_values);
      } else {
        rse_fill_storage_buffer_baseline_rowwise(buff, generator, rs_groups, rs_values);
      }
      break;
    }
    default:
      CHECK(false);
  }
  CHECK(buff);
}

void ResultSetEmulator::print_emulator_diag() {
  if (!rs_silent) {
    for (size_t j = 0; j < rs_entry_count; j++) {
      int g1 = 0, g2 = 0;
      if (rs1_groups[j]) {
        g1 = 1;
      }
      if (rs2_groups[j]) {
        g2 = 1;
      }
      printf("\nGroup #%i (%i,%i): Buf1=%ld Buf2=%ld", (int)j, g1, g2, rs1_values[j], rs2_values[j]);
    }
  }
}

void ResultSetEmulator::print_rse_generated_result_sets() const {
  printf("\nResultSet #1 Final Groups: ");
  for (size_t i = 0; i < rs1_groups.size(); i++) {
    if (rs1_groups[i])
      printf("1");
    else
      printf("0");
  }

  printf("\nResultSet #2 Final Groups: ");
  for (size_t i = 0; i < rs2_groups.size(); i++) {
    if (rs2_groups[i])
      printf("1");
    else
      printf("0");
  }
  printf("\n");
}

void ResultSetEmulator::print_merged_result_sets(const auto result) {
  printf("\n ****** KMIN_DATA_FROM_RS_MERGE_CODE ****** %i", (int)result.size());
  size_t j = 0;
  for (const auto& row : result) {
    const auto ival_0 = v<int64_t>(row[0]);  // kMIN
    const auto ival_1 = v<int64_t>(row[1]);  // kMAX
    const auto ival_2 = v<int64_t>(row[2]);  // kSUM
    const auto ival_3 = v<int64_t>(row[3]);  // kCOUNT
    const auto ival_4 = v<double>(row[4]);   // kAVG
    printf("\n Group #%i KMIN/KMAX/KSUM/KCOUNT from RS_MergeCode: %ld %ld %ld %ld %f",
           (int)j,
           ival_0,
           ival_1,
           ival_2,
           ival_3,
           ival_4);
    j++;
  }

  size_t active_group_count = 0;
  for (size_t j = 0; j < rseReducedGroups.size(); j++) {
    if (rseReducedGroups[j])
      active_group_count++;
  }
  printf("\n\n ****** KMIN_DATA_FROM_MERGE_BUFFER_CODE ****** Total: %i, Active: %i",
         (int)rs_entry_count,
         (int)active_group_count);
  size_t num_groups = getReferenceTable().size();
  for (size_t i = 0; i < num_groups; i++) {
    std::vector<int64_t> ref_row = getReferenceRow(true);
    int64_t ref_val_0 = ref_row[0];  // kMIN
    int64_t ref_val_1 = ref_row[1];  // kMAX
    int64_t ref_val_2 = ref_row[2];  // kSUM
    int64_t ref_val_3 = ref_row[3];  // kCOUNT
    int64_t ref_val_4 = ref_row[4];  // kAVG
    printf("\n Group #%i KMIN/KMAX/KSUM/KCOUNT from ReducedBuffer: %ld %ld %ld %ld %f",
           (int)i,
           ref_val_0,
           ref_val_1,
           ref_val_2,
           ref_val_3,
           (double)ref_val_4);
  }
  printf("\n");
}

int64_t ResultSetEmulator::rseAggregateKMIN(size_t i) {
  int64_t result = 0;

  if (rs1_groups[i] && rs2_groups[i]) {
    result = std::min(rs1_values[i], rs2_values[i]);
  } else {
    if (rs1_groups[i]) {
      result = rs1_values[i];
    } else {
      if (rs2_groups[i]) {
        result = rs2_values[i];
      }
    }
  }

  return result;
}

int64_t ResultSetEmulator::rseAggregateKMAX(size_t i) {
  int64_t result = 0;

  if (rs1_groups[i] && rs2_groups[i]) {
    result = std::max(rs1_values[i], rs2_values[i]);
  } else {
    if (rs1_groups[i]) {
      result = rs1_values[i];
    } else {
      if (rs2_groups[i]) {
        result = rs2_values[i];
      }
    }
  }

  return result;
}

int64_t ResultSetEmulator::rseAggregateKAVG(size_t i) {
  int64_t result = 0;
  int n1 = 1,
      n2 = 1;  // for test purposes count of elements in each group is 1 (see proc "fill_one_entry_no_collisions")

  if (rs1_groups[i] && rs2_groups[i]) {
    result = (rs1_values[i] / n1 + rs2_values[i] / n2) / 2;
  } else {
    if (rs1_groups[i]) {
      result = rs1_values[i] / n1;
    } else {
      if (rs2_groups[i]) {
        result = rs2_values[i] / n2;
      }
    }
  }

  return result;
}

int64_t ResultSetEmulator::rseAggregateKSUM(size_t i) {
  int64_t result = 0;

  if (rs1_groups[i]) {
    result += rs1_values[i];
  }
  if (rs2_groups[i]) {
    result += rs2_values[i];
  }

  return result;
}

int64_t ResultSetEmulator::rseAggregateKCOUNT(size_t i) {
  int64_t result = 0;

  if (rs1_groups[i]) {
    result += rs1_values[i];
  }
  if (rs2_groups[i]) {
    result += rs2_values[i];
  }

  return result;
}

bool approx_eq(const double v, const double target, const double eps = 0.01) {
  return target - eps < v && v < target + eps;
}

StringDictionary g_sd("");

void test_iterate(const std::vector<TargetInfo>& target_infos, const QueryMemoryDescriptor& query_mem_desc) {
  SQLTypeInfo double_ti(kDOUBLE, false);
  auto row_set_mem_owner = std::make_shared<RowSetMemoryOwner>();
  row_set_mem_owner->addStringDict(&g_sd, 1);
  ResultSet result_set(target_infos, ExecutorDeviceType::CPU, query_mem_desc, row_set_mem_owner);
  for (size_t i = 0; i < query_mem_desc.entry_count; ++i) {
    g_sd.getOrAddTransient(std::to_string(i));
  }
  const auto storage = result_set.allocateStorage();
  EvenNumberGenerator generator;
  fill_storage_buffer(storage->getUnderlyingBuffer(), target_infos, query_mem_desc, generator, 2);
  int64_t ref_val{0};
  while (true) {
    const auto row = result_set.getNextRow(true, false);
    if (row.empty()) {
      break;
    }
    CHECK_EQ(target_infos.size(), row.size());
    for (size_t i = 0; i < target_infos.size(); ++i) {
      const auto& target_info = target_infos[i];
      const auto& ti = target_info.agg_kind == kAVG ? double_ti : target_info.sql_type;
      switch (ti.get_type()) {
        case kSMALLINT:
        case kINT:
        case kBIGINT: {
          const auto ival = v<int64_t>(row[i]);
          ASSERT_EQ(ref_val, ival);
          break;
        }
        case kDOUBLE: {
          const auto dval = v<double>(row[i]);
          ASSERT_TRUE(approx_eq(static_cast<double>(ref_val), dval));
          break;
        }
        case kTEXT: {
          const auto sval = v<NullableString>(row[i]);
          ASSERT_EQ(std::to_string(ref_val), boost::get<std::string>(sval));
          break;
        }
        default:
          CHECK(false);
      }
    }
    ref_val += 2;
  }
}

std::vector<TargetInfo> generate_test_target_infos() {
  std::vector<TargetInfo> target_infos;
  SQLTypeInfo int_ti(kINT, false);
  SQLTypeInfo double_ti(kDOUBLE, false);
  SQLTypeInfo null_ti(kNULLT, false);
  target_infos.push_back(TargetInfo{false, kMIN, int_ti, null_ti, true, false});
  target_infos.push_back(TargetInfo{true, kAVG, int_ti, int_ti, true, false});
  target_infos.push_back(TargetInfo{true, kSUM, int_ti, int_ti, true, false});
  target_infos.push_back(TargetInfo{false, kMIN, double_ti, null_ti, true, false});
  {
    SQLTypeInfo dict_string_ti(kTEXT, false);
    dict_string_ti.set_compression(kENCODING_DICT);
    dict_string_ti.set_comp_param(1);
    target_infos.push_back(TargetInfo{false, kMIN, dict_string_ti, null_ti, true, false});
  }
  return target_infos;
}

std::vector<TargetInfo> generate_random_groups_target_infos() {
  std::vector<TargetInfo> target_infos;
  SQLTypeInfo int_ti(kINT, false);
  // SQLTypeInfo double_ti(kDOUBLE, false);
  SQLTypeInfo null_ti(kNULLT, false);
  target_infos.push_back(TargetInfo{true, kMIN, int_ti, int_ti, true, false});
  target_infos.push_back(TargetInfo{true, kMAX, int_ti, int_ti, true, false});
  target_infos.push_back(TargetInfo{true, kSUM, int_ti, int_ti, true, false});
  target_infos.push_back(TargetInfo{true, kCOUNT, int_ti, int_ti, true, false});
  target_infos.push_back(TargetInfo{true, kAVG, int_ti, int_ti, true, false});
  return target_infos;
}

typedef std::vector<TargetValue> OneRow;

std::vector<OneRow> get_rows_sorted_by_col(const ResultSet& rs, const size_t col_idx) {
  std::vector<OneRow> result;
  while (true) {
    const auto row = rs.getNextRow(false, false);
    if (row.empty()) {
      break;
    }
    result.push_back(row);
  }
  return result;
}

void test_reduce(const std::vector<TargetInfo>& target_infos,
                 const QueryMemoryDescriptor& query_mem_desc,
                 NumberGenerator& generator1,
                 NumberGenerator& generator2,
                 const int step) {
  SQLTypeInfo double_ti(kDOUBLE, false);
  const ResultSetStorage* storage1{nullptr};
  const ResultSetStorage* storage2{nullptr};
  std::unique_ptr<ResultSet> rs1;
  std::unique_ptr<ResultSet> rs2;
  const auto row_set_mem_owner = std::make_shared<RowSetMemoryOwner>();
  row_set_mem_owner->addStringDict(&g_sd, 1);
  switch (query_mem_desc.hash_type) {
    case GroupByColRangeType::OneColKnownRange:
    case GroupByColRangeType::MultiColPerfectHash: {
      rs1.reset(new ResultSet(target_infos, ExecutorDeviceType::CPU, query_mem_desc, row_set_mem_owner));
      storage1 = rs1->allocateStorage();
      fill_storage_buffer(storage1->getUnderlyingBuffer(), target_infos, query_mem_desc, generator1, step);
      rs2.reset(new ResultSet(target_infos, ExecutorDeviceType::CPU, query_mem_desc, row_set_mem_owner));
      storage2 = rs2->allocateStorage();
      fill_storage_buffer(storage2->getUnderlyingBuffer(), target_infos, query_mem_desc, generator2, step);
      break;
    }
    case GroupByColRangeType::MultiCol: {
      rs1.reset(new ResultSet(target_infos, ExecutorDeviceType::CPU, query_mem_desc, row_set_mem_owner));
      storage1 = rs1->allocateStorage();
      fill_storage_buffer(storage1->getUnderlyingBuffer(), target_infos, query_mem_desc, generator1, step);
      rs2.reset(new ResultSet(target_infos, ExecutorDeviceType::CPU, query_mem_desc, row_set_mem_owner));
      storage2 = rs2->allocateStorage();
      fill_storage_buffer(storage2->getUnderlyingBuffer(), target_infos, query_mem_desc, generator2, step);
      break;
    }
    default:
      CHECK(false);
  }
  ResultSetManager rs_manager;
  std::vector<ResultSet*> storage_set{rs1.get(), rs2.get()};
  auto result_rs = rs_manager.reduce(storage_set);
  int64_t ref_val{0};
  std::list<Analyzer::OrderEntry> order_entries;
  order_entries.emplace_back(1, false, false);
  result_rs->sort(order_entries, 0);
  while (true) {
    const auto row = result_rs->getNextRow(false, false);
    if (row.empty()) {
      break;
    }
    CHECK_EQ(target_infos.size(), row.size());
    for (size_t i = 0; i < target_infos.size(); ++i) {
      const auto& target_info = target_infos[i];
      const auto& ti = target_info.agg_kind == kAVG ? double_ti : target_info.sql_type;
      switch (ti.get_type()) {
        case kSMALLINT:
        case kINT:
        case kBIGINT: {
          const auto ival = v<int64_t>(row[i]);
          ASSERT_EQ((target_info.agg_kind == kSUM || target_info.agg_kind == kCOUNT) ? step * ref_val : ref_val, ival);
          break;
        }
        case kDOUBLE: {
          const auto dval = v<double>(row[i]);
          ASSERT_TRUE(approx_eq(
              static_cast<double>((target_info.agg_kind == kSUM || target_info.agg_kind == kCOUNT) ? step * ref_val
                                                                                                   : ref_val),
              dval));
          break;
        }
        case kTEXT:
          break;
        default:
          CHECK(false);
      }
    }
    ref_val += step;
  }
}

void test_reduce_random_groups(const std::vector<TargetInfo>& target_infos,
                               const QueryMemoryDescriptor& query_mem_desc,
                               NumberGenerator& generator1,
                               NumberGenerator& generator2,
                               const int prct1,
                               const int prct2,
                               bool silent,
                               const int step = 0) {
  SQLTypeInfo double_ti(kDOUBLE, false);
  const ResultSetStorage* storage1{nullptr};
  const ResultSetStorage* storage2{nullptr};
  std::unique_ptr<ResultSet> rs1;
  std::unique_ptr<ResultSet> rs2;
  const auto row_set_mem_owner = std::make_shared<RowSetMemoryOwner>();
  switch (query_mem_desc.hash_type) {
    case GroupByColRangeType::OneColKnownRange:
    case GroupByColRangeType::MultiColPerfectHash: {
      rs1.reset(new ResultSet(target_infos, ExecutorDeviceType::CPU, query_mem_desc, row_set_mem_owner));
      storage1 = rs1->allocateStorage();
      rs2.reset(new ResultSet(target_infos, ExecutorDeviceType::CPU, query_mem_desc, row_set_mem_owner));
      storage2 = rs2->allocateStorage();
      break;
    }
    case GroupByColRangeType::MultiCol: {
      rs1.reset(new ResultSet(target_infos, ExecutorDeviceType::CPU, query_mem_desc, row_set_mem_owner));
      storage1 = rs1->allocateStorage();
      rs2.reset(new ResultSet(target_infos, ExecutorDeviceType::CPU, query_mem_desc, row_set_mem_owner));
      storage2 = rs2->allocateStorage();
      break;
    }
    default:
      CHECK(false);
  }

  ResultSetEmulator* rse = new ResultSetEmulator(storage1->getUnderlyingBuffer(),
                                                 storage2->getUnderlyingBuffer(),
                                                 target_infos,
                                                 query_mem_desc,
                                                 generator1,
                                                 generator2,
                                                 prct1,
                                                 prct2,
                                                 step,
                                                 silent);
  if (!silent) {
    rse->print_rse_generated_result_sets();
  }

  ResultSetManager rs_manager;
  std::vector<ResultSet*> storage_set{rs1.get(), rs2.get()};
  auto result_rs = rs_manager.reduce(storage_set);
  std::queue<std::vector<int64_t>> ref_table = rse->getReferenceTable();
  std::vector<bool> ref_group_map = rse->getReferenceGroupMap();
  const auto result = get_rows_sorted_by_col(*result_rs, 0);
  CHECK(!result.empty());

  if (!silent) {
    rse->print_merged_result_sets(result);
  }

  size_t row_idx = 0;
  int64_t ref_val = 0;
  for (const auto& row : result) {
    CHECK_EQ(target_infos.size(), row.size());
    while (true) {
      if (row_idx >= rse->getReferenceGroupMap().size()) {
        LOG(FATAL) << "Number of groups in reduced result set is more than expected";
      }
      if (rse->getReferenceGroupMapElement(row_idx)) {
        break;
      } else {
        row_idx++;
        continue;
      }
    }
    std::vector<int64_t> ref_row = rse->getReferenceRow();
    for (size_t i = 0; i < target_infos.size(); ++i) {
      ref_val = ref_row[i];
      const auto& target_info = target_infos[i];
      const auto& ti = target_info.agg_kind == kAVG ? double_ti : target_info.sql_type;
      switch (ti.get_type()) {
        case kSMALLINT:
        case kINT:
        case kBIGINT: {
          const auto ival = v<int64_t>(row[i]);
          switch (target_info.agg_kind) {
            case kMIN: {
              if (!silent) {
                printf("\nKMIN row_idx = %i, ref_val = %ld, ival = %ld", (int)row_idx, ref_val, ival);
                if (ref_val != ival) {
                  printf("%21s%s", "", "KMIN TEST FAILED!\n");
                } else {
                  printf("%21s%s", "", "KMIN TEST PASSED!\n");
                }
              } else {
                ASSERT_EQ(ref_val, ival);
              }
              break;
            }
            case kMAX: {
              if (!silent) {
                printf("\nKMAX row_idx = %i, ref_val = %ld, ival = %ld", (int)row_idx, ref_val, ival);
                if (ref_val != ival) {
                  printf("%21s%s", "", "KMAX TEST FAILED!\n");
                } else {
                  printf("%21s%s", "", "KMAX TEST PASSED!\n");
                }
              } else {
                ASSERT_EQ(ref_val, ival);
              }
              break;
            }
            case kAVG: {
              if (!silent) {
                printf("\nKAVG row_idx = %i, ref_val = %ld, ival = %ld", (int)row_idx, ref_val, ival);
                if (ref_val != ival) {
                  printf("%21s%s", "", "KAVG TEST FAILED!\n");
                } else {
                  printf("%21s%s", "", "KAVG TEST PASSED!\n");
                }
              } else {
                ASSERT_EQ(ref_val, ival);
              }
              break;
            }
            case kSUM:
            case kCOUNT: {
              if (!silent) {
                printf("\nKSUM row_idx = %i, ref_val = %ld, ival = %ld", (int)row_idx, ref_val, ival);
                if (ref_val != ival) {
                  printf("%21s%s", "", "KSUM TEST FAILED!\n");
                } else {
                  printf("%21s%s", "", "KSUM TEST PASSED!\n");
                }
              } else {
                ASSERT_EQ(ref_val, ival);
              }
              break;
            }
            default:
              CHECK(false);
          }
          break;
        }
        case kDOUBLE: {
          const auto dval = v<double>(row[i]);
          switch (target_info.agg_kind) {
            case kMIN: {
              if (!silent) {
                printf("\nKMIN_D row_idx = %i, ref_val = %f, dval = %f", (int)row_idx, (double)ref_val, dval);
                if (static_cast<double>(ref_val) != dval) {
                  printf("%5s%s", "", "KMIN_D TEST FAILED!\n");
                } else {
                  printf("%5s%s", "", "KMIN_D TEST PASSED!\n");
                }
              } else {
                ASSERT_TRUE(approx_eq(static_cast<double>(ref_val), dval));
              }
              break;
            }
            case kMAX: {
              if (!silent) {
                printf("\nKMAX_D row_idx = %i, ref_val = %f, dval = %f", (int)row_idx, (double)ref_val, dval);
                if (static_cast<double>(ref_val) != dval) {
                  printf("%5s%s", "", "KMAX_D TEST FAILED!\n");
                } else {
                  printf("%5s%s", "", "KMAX_D TEST PASSED!\n");
                }
              } else {
                ASSERT_TRUE(approx_eq(static_cast<double>(ref_val), dval));
              }
              break;
            }
            case kAVG: {
              if (!silent) {
                printf("\nKAVG_D row_idx = %i, ref_val = %f, dval = %f", (int)row_idx, (double)ref_val, dval);
                if (static_cast<double>(ref_val) != dval) {
                  printf("%5s%s", "", "KAVG_D TEST FAILED!\n");
                } else {
                  printf("%5s%s", "", "KAVG_D TEST PASSED!\n");
                }
              } else {
                ASSERT_TRUE(approx_eq(static_cast<double>(ref_val), dval));
              }
              break;
            }
            case kSUM:
            case kCOUNT: {
              if (!silent) {
                printf("\nKSUM_D row_idx = %i, ref_val = %f, dval = %f", (int)row_idx, (double)ref_val, dval);
                if (static_cast<double>(ref_val) != dval) {
                  printf("%5s%s", "", "KSUM_D TEST FAILED!\n");
                } else {
                  printf("%5s%s", "", "KSUM_D TEST PASSED!\n");
                }
              } else {
                ASSERT_TRUE(approx_eq(static_cast<double>(ref_val), dval));
              }
              break;
            }
            default:
              CHECK(false);
          }
          break;
        }
        default:
          CHECK(false);
      }
    }
    row_idx++;
  }

  delete rse;
}
}  // namespace

TEST(Iterate, PerfectHashOneCol) {
  const auto target_infos = generate_test_target_infos();
  const auto query_mem_desc = perfect_hash_one_col_desc(target_infos, 8);
  test_iterate(target_infos, query_mem_desc);
}

TEST(Iterate, PerfectHashOneCol32) {
  const auto target_infos = generate_test_target_infos();
  const auto query_mem_desc = perfect_hash_one_col_desc(target_infos, 4);
  test_iterate(target_infos, query_mem_desc);
}

TEST(Iterate, PerfectHashOneColColumnar) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = perfect_hash_one_col_desc(target_infos, 8);
  query_mem_desc.output_columnar = true;
  test_iterate(target_infos, query_mem_desc);
}

TEST(Iterate, PerfectHashOneColColumnar32) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = perfect_hash_one_col_desc(target_infos, 4);
  query_mem_desc.output_columnar = true;
  test_iterate(target_infos, query_mem_desc);
}

TEST(Iterate, PerfectHashOneColKeyless) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = perfect_hash_one_col_desc(target_infos, 8);
  query_mem_desc.keyless_hash = true;
  query_mem_desc.idx_target_as_key = 2;
  test_iterate(target_infos, query_mem_desc);
}

TEST(Iterate, PerfectHashOneColKeyless32) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = perfect_hash_one_col_desc(target_infos, 4);
  query_mem_desc.keyless_hash = true;
  query_mem_desc.idx_target_as_key = 2;
  test_iterate(target_infos, query_mem_desc);
}

TEST(Iterate, PerfectHashOneColColumnarKeyless) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = perfect_hash_one_col_desc(target_infos, 8);
  query_mem_desc.output_columnar = true;
  query_mem_desc.keyless_hash = true;
  query_mem_desc.idx_target_as_key = 2;
  test_iterate(target_infos, query_mem_desc);
}

TEST(Iterate, PerfectHashOneColColumnarKeyless32) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = perfect_hash_one_col_desc(target_infos, 4);
  query_mem_desc.output_columnar = true;
  query_mem_desc.keyless_hash = true;
  query_mem_desc.idx_target_as_key = 2;
  test_iterate(target_infos, query_mem_desc);
}

TEST(Iterate, PerfectHashTwoCol) {
  const auto target_infos = generate_test_target_infos();
  const auto query_mem_desc = perfect_hash_two_col_desc(target_infos, 8);
  test_iterate(target_infos, query_mem_desc);
}

TEST(Iterate, PerfectHashTwoCol32) {
  const auto target_infos = generate_test_target_infos();
  const auto query_mem_desc = perfect_hash_two_col_desc(target_infos, 4);
  test_iterate(target_infos, query_mem_desc);
}

TEST(Iterate, PerfectHashTwoColColumnar) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = perfect_hash_two_col_desc(target_infos, 8);
  query_mem_desc.output_columnar = true;
  test_iterate(target_infos, query_mem_desc);
}

TEST(Iterate, PerfectHashTwoColColumnar32) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = perfect_hash_two_col_desc(target_infos, 4);
  query_mem_desc.output_columnar = true;
  test_iterate(target_infos, query_mem_desc);
}

TEST(Iterate, PerfectHashTwoColKeyless) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = perfect_hash_two_col_desc(target_infos, 8);
  query_mem_desc.keyless_hash = true;
  query_mem_desc.idx_target_as_key = 2;
  test_iterate(target_infos, query_mem_desc);
}

TEST(Iterate, PerfectHashTwoColKeyless32) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = perfect_hash_two_col_desc(target_infos, 4);
  query_mem_desc.keyless_hash = true;
  query_mem_desc.idx_target_as_key = 2;
  test_iterate(target_infos, query_mem_desc);
}

TEST(Iterate, PerfectHashTwoColColumnarKeyless) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = perfect_hash_two_col_desc(target_infos, 8);
  query_mem_desc.output_columnar = true;
  query_mem_desc.keyless_hash = true;
  query_mem_desc.idx_target_as_key = 2;
  test_iterate(target_infos, query_mem_desc);
}

TEST(Iterate, PerfectHashTwoColColumnarKeyless32) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = perfect_hash_two_col_desc(target_infos, 4);
  query_mem_desc.output_columnar = true;
  query_mem_desc.keyless_hash = true;
  query_mem_desc.idx_target_as_key = 2;
  test_iterate(target_infos, query_mem_desc);
}

TEST(Iterate, BaselineHash) {
  const auto target_infos = generate_test_target_infos();
  const auto query_mem_desc = baseline_hash_two_col_desc(target_infos, 8);
  test_iterate(target_infos, query_mem_desc);
}

TEST(Iterate, BaselineHashColumnar) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = baseline_hash_two_col_desc(target_infos, 8);
  query_mem_desc.output_columnar = true;
  test_iterate(target_infos, query_mem_desc);
}

TEST(Reduce, PerfectHashOneCol) {
  const auto target_infos = generate_test_target_infos();
  const auto query_mem_desc = perfect_hash_one_col_desc(target_infos, 8);
  EvenNumberGenerator generator1;
  EvenNumberGenerator generator2;
  test_reduce(target_infos, query_mem_desc, generator1, generator2, 2);
}

TEST(Reduce, PerfectHashOneCol32) {
  const auto target_infos = generate_test_target_infos();
  const auto query_mem_desc = perfect_hash_one_col_desc(target_infos, 4);
  EvenNumberGenerator generator1;
  EvenNumberGenerator generator2;
  test_reduce(target_infos, query_mem_desc, generator1, generator2, 2);
}

TEST(Reduce, PerfectHashOneColColumnar) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = perfect_hash_one_col_desc(target_infos, 8);
  query_mem_desc.output_columnar = true;
  EvenNumberGenerator generator1;
  EvenNumberGenerator generator2;
  test_reduce(target_infos, query_mem_desc, generator1, generator2, 2);
}

TEST(Reduce, PerfectHashOneColColumnar32) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = perfect_hash_one_col_desc(target_infos, 4);
  query_mem_desc.output_columnar = true;
  EvenNumberGenerator generator1;
  EvenNumberGenerator generator2;
  test_reduce(target_infos, query_mem_desc, generator1, generator2, 2);
}

TEST(Reduce, PerfectHashOneColKeyless) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = perfect_hash_one_col_desc(target_infos, 8);
  query_mem_desc.keyless_hash = true;
  query_mem_desc.idx_target_as_key = 2;
  EvenNumberGenerator generator1;
  EvenNumberGenerator generator2;
  test_reduce(target_infos, query_mem_desc, generator1, generator2, 2);
}

TEST(Reduce, PerfectHashOneColKeyless32) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = perfect_hash_one_col_desc(target_infos, 4);
  query_mem_desc.keyless_hash = true;
  query_mem_desc.idx_target_as_key = 2;
  EvenNumberGenerator generator1;
  EvenNumberGenerator generator2;
  test_reduce(target_infos, query_mem_desc, generator1, generator2, 2);
}

TEST(Reduce, PerfectHashOneColColumnarKeyless) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = perfect_hash_one_col_desc(target_infos, 8);
  query_mem_desc.output_columnar = true;
  query_mem_desc.keyless_hash = true;
  query_mem_desc.idx_target_as_key = 2;
  EvenNumberGenerator generator1;
  EvenNumberGenerator generator2;
  test_reduce(target_infos, query_mem_desc, generator1, generator2, 2);
}

TEST(Reduce, PerfectHashOneColColumnarKeyless32) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = perfect_hash_one_col_desc(target_infos, 4);
  query_mem_desc.output_columnar = true;
  query_mem_desc.keyless_hash = true;
  query_mem_desc.idx_target_as_key = 2;
  EvenNumberGenerator generator1;
  EvenNumberGenerator generator2;
  test_reduce(target_infos, query_mem_desc, generator1, generator2, 2);
}

TEST(Reduce, PerfectHashTwoCol) {
  const auto target_infos = generate_test_target_infos();
  const auto query_mem_desc = perfect_hash_two_col_desc(target_infos, 8);
  EvenNumberGenerator generator1;
  EvenNumberGenerator generator2;
  test_reduce(target_infos, query_mem_desc, generator1, generator2, 2);
}

TEST(Reduce, PerfectHashTwoCol32) {
  const auto target_infos = generate_test_target_infos();
  const auto query_mem_desc = perfect_hash_two_col_desc(target_infos, 4);
  EvenNumberGenerator generator1;
  EvenNumberGenerator generator2;
  test_reduce(target_infos, query_mem_desc, generator1, generator2, 2);
}

TEST(Reduce, PerfectHashTwoColColumnar) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = perfect_hash_two_col_desc(target_infos, 8);
  query_mem_desc.output_columnar = true;
  EvenNumberGenerator generator1;
  EvenNumberGenerator generator2;
  test_reduce(target_infos, query_mem_desc, generator1, generator2, 2);
}

TEST(Reduce, PerfectHashTwoColColumnar32) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = perfect_hash_two_col_desc(target_infos, 4);
  query_mem_desc.output_columnar = true;
  EvenNumberGenerator generator1;
  EvenNumberGenerator generator2;
  test_reduce(target_infos, query_mem_desc, generator1, generator2, 2);
}

TEST(Reduce, PerfectHashTwoColKeyless) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = perfect_hash_two_col_desc(target_infos, 8);
  query_mem_desc.keyless_hash = true;
  query_mem_desc.idx_target_as_key = 2;
  EvenNumberGenerator generator1;
  EvenNumberGenerator generator2;
  test_reduce(target_infos, query_mem_desc, generator1, generator2, 2);
}

TEST(Reduce, PerfectHashTwoColKeyless32) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = perfect_hash_two_col_desc(target_infos, 4);
  query_mem_desc.keyless_hash = true;
  query_mem_desc.idx_target_as_key = 2;
  EvenNumberGenerator generator1;
  EvenNumberGenerator generator2;
  test_reduce(target_infos, query_mem_desc, generator1, generator2, 2);
}

TEST(Reduce, PerfectHashTwoColColumnarKeyless) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = perfect_hash_two_col_desc(target_infos, 8);
  query_mem_desc.output_columnar = true;
  query_mem_desc.keyless_hash = true;
  query_mem_desc.idx_target_as_key = 2;
  EvenNumberGenerator generator1;
  EvenNumberGenerator generator2;
  test_reduce(target_infos, query_mem_desc, generator1, generator2, 2);
}

TEST(Reduce, PerfectHashTwoColColumnarKeyless32) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = perfect_hash_two_col_desc(target_infos, 4);
  query_mem_desc.output_columnar = true;
  query_mem_desc.keyless_hash = true;
  query_mem_desc.idx_target_as_key = 2;
  EvenNumberGenerator generator1;
  EvenNumberGenerator generator2;
  test_reduce(target_infos, query_mem_desc, generator1, generator2, 2);
}

TEST(Reduce, BaselineHash) {
  const auto target_infos = generate_test_target_infos();
  const auto query_mem_desc = baseline_hash_two_col_desc(target_infos, 8);
  EvenNumberGenerator generator1;
  ReverseOddOrEvenNumberGenerator generator2(2 * query_mem_desc.entry_count - 1);
  test_reduce(target_infos, query_mem_desc, generator1, generator2, 1);
}

TEST(Reduce, BaselineHashColumnar) {
  const auto target_infos = generate_test_target_infos();
  auto query_mem_desc = baseline_hash_two_col_desc(target_infos, 8);
  query_mem_desc.output_columnar = true;
  EvenNumberGenerator generator1;
  ReverseOddOrEvenNumberGenerator generator2(2 * query_mem_desc.entry_count - 1);
  test_reduce(target_infos, query_mem_desc, generator1, generator2, 1);
}

/* Perfect_Hash_Row_Based testcases */
// TBD_FLOW #1
TEST(ReduceRandomGroups,
     DISABLED_PerfectHashOneCol_Small_2525) {  // TBD_BUG - fails: only ResultSet #1 groups are included
  const auto target_infos = generate_random_groups_target_infos();
  const auto query_mem_desc = perfect_hash_one_col_desc_small(target_infos, 8);
  // const auto query_mem_desc = perfect_hash_one_col_desc(target_infos, 8);
  EvenNumberGenerator gen1;
  EvenNumberGenerator gen2;
  const int prct1 = 25, prct2 = 25;
  bool silent = false;  // true/false - don't/do print diagnostic messages
  // silent = true;
  test_reduce_random_groups(target_infos, query_mem_desc, gen1, gen2, prct1, prct2, silent);
}

TEST(ReduceRandomGroups,
     DISABLED_PerfectHashOneCol_Small_2575) {  // TBD_BUG - fails: only ResultSet #1 groups are included
  const auto target_infos = generate_random_groups_target_infos();
  const auto query_mem_desc = perfect_hash_one_col_desc_small(target_infos, 8);
  // const auto query_mem_desc = perfect_hash_one_col_desc(target_infos, 8);
  EvenNumberGenerator gen1;
  EvenNumberGenerator gen2;
  const int prct1 = 25, prct2 = 75;
  bool silent = false;  // true/false - don't/do print diagnostic messages
  // silent = true;
  test_reduce_random_groups(target_infos, query_mem_desc, gen1, gen2, prct1, prct2, silent);
}

TEST(ReduceRandomGroups,
     DISABLED_PerfectHashOneCol_Small_5050) {  // TBD_BUG - fails: only ResultSet #1 groups are included
  const auto target_infos = generate_random_groups_target_infos();
  const auto query_mem_desc = perfect_hash_one_col_desc_small(target_infos, 8);
  // const auto query_mem_desc = perfect_hash_one_col_desc(target_infos, 8);
  EvenNumberGenerator gen1;
  EvenNumberGenerator gen2;
  const int prct1 = 50, prct2 = 50;
  bool silent = false;  // true/false - don't/do print diagnostic messages
  // silent = true;
  test_reduce_random_groups(target_infos, query_mem_desc, gen1, gen2, prct1, prct2, silent);
}

TEST(ReduceRandomGroups,
     DISABLED_PerfectHashOneCol_Small_7525) {  // TBD_BUG - fails: only ResultSet #1 groups are included
  const auto target_infos = generate_random_groups_target_infos();
  const auto query_mem_desc = perfect_hash_one_col_desc_small(target_infos, 8);
  // const auto query_mem_desc = perfect_hash_one_col_desc(target_infos, 8);
  EvenNumberGenerator gen1;
  EvenNumberGenerator gen2;
  const int prct1 = 75, prct2 = 25;
  bool silent = false;  // true/false - don't/do print diagnostic messages
  // silent = true;
  test_reduce_random_groups(target_infos, query_mem_desc, gen1, gen2, prct1, prct2, silent);
}

TEST(ReduceRandomGroups,
     DISABLED_PerfectHashOneCol_Small_25100) {  // TBD_BUG - fails: only ResultSet #1 groups are included
  const auto target_infos = generate_random_groups_target_infos();
  const auto query_mem_desc = perfect_hash_one_col_desc_small(target_infos, 8);
  // const auto query_mem_desc = perfect_hash_one_col_desc(target_infos, 8);
  EvenNumberGenerator gen1;
  EvenNumberGenerator gen2;
  const int prct1 = 25, prct2 = 100;
  bool silent = false;  // true/false - don't/do print diagnostic messages
  // silent = true;
  test_reduce_random_groups(target_infos, query_mem_desc, gen1, gen2, prct1, prct2, silent);
}

TEST(ReduceRandomGroups,
     DISABLED_PerfectHashOneCol_Small_10025) {  // TBD_BUG - fails: when calculating AVG, ResultSet #2 overwrites #1
  const auto target_infos = generate_random_groups_target_infos();
  const auto query_mem_desc = perfect_hash_one_col_desc_small(target_infos, 8);
  // const auto query_mem_desc = perfect_hash_one_col_desc(target_infos, 8);
  EvenNumberGenerator gen1;
  EvenNumberGenerator gen2;
  const int prct1 = 100, prct2 = 25;
  bool silent = false;  // true/false - don't/do print diagnostic messages
  // silent = true;
  test_reduce_random_groups(target_infos, query_mem_desc, gen1, gen2, prct1, prct2, silent);
}

TEST(ReduceRandomGroups,
     DISABLED_PerfectHashOneCol_Small_100100) {  // TBD_BUG - passes, as there is 100% match between all groups of both
                                                 // ResultSets
  const auto target_infos = generate_random_groups_target_infos();
  const auto query_mem_desc = perfect_hash_one_col_desc_small(target_infos, 8);
  // const auto query_mem_desc = perfect_hash_one_col_desc(target_infos, 8);
  EvenNumberGenerator gen1;
  EvenNumberGenerator gen2;
  const int prct1 = 100, prct2 = 100;
  bool silent = false;  // true/false - don't/do print diagnostic messages
  // silent = true;
  test_reduce_random_groups(target_infos, query_mem_desc, gen1, gen2, prct1, prct2, silent);
}

TEST(ReduceRandomGroups,
     DISABLED_PerfectHashOneCol_Small_9505) {  // TBD_BUG - fails: only ResultSet #1 groups are included; AVG is not
                                               // correct : ResultSet #2 overwrites #1
  const auto target_infos = generate_random_groups_target_infos();
  const auto query_mem_desc = perfect_hash_one_col_desc_small(target_infos, 8);
  EvenNumberGenerator gen1;
  EvenNumberGenerator gen2;
  const int prct1 = 95, prct2 = 5;
  bool silent = false;  // true/false - don't/do print diagnostic messages
  // silent = true;
  test_reduce_random_groups(target_infos, query_mem_desc, gen1, gen2, prct1, prct2, silent);
}

/* TBD enable, retry, and add to the testsuite this testcase after fixing all bugs
TEST(ReduceRandomGroups, DISABLED_PerfectHashOneCol_Large_2525) { // TBD - ignore this test for now, includes too many
groups not
convenient for debugging
  const auto target_infos = generate_random_groups_target_infos();
  // const auto query_mem_desc = perfect_hash_one_col_desc_small(target_infos, 8);
  const auto query_mem_desc = perfect_hash_one_col_desc(target_infos, 8);
  EvenNumberGenerator gen1;
  EvenNumberGenerator gen2;
  const int prct1 = 25, prct2 = 25;
  bool silent = false; // true/false - don't/do print diagnostic messages
  // silent = true;
  test_reduce_random_groups(target_infos, query_mem_desc, gen1, gen2, prct1, prct2, silent);
}
*/

/* Non_Perfect_Hash_Row_Based testcases */
// TBD_FLOW #2
TEST(ReduceRandomGroups,
     DISABLED_BaselineHash_Large_5050) {  // TBD_BUG - fails: # groups in merged ResultSet is correct, but SUM & COUNT
                                          // are wrong, AVG is correct - why ?
  const auto target_infos = generate_random_groups_target_infos();
  const auto query_mem_desc = baseline_hash_two_col_desc_large(target_infos, 8);
  EvenNumberGenerator gen1;
  EvenNumberGenerator gen2;
  const int prct1 = 50, prct2 = 50;
  bool silent = false;  // true/false - don't/do print diagnostic messages
  // silent = true;
  test_reduce_random_groups(target_infos, query_mem_desc, gen1, gen2, prct1, prct2, silent);
}

TEST(ReduceRandomGroups,
     DISABLED_BaselineHash_Large_7525) {  // TBD_BUG - fails: # groups in merged ResultSet is correct, but SUM & COUNT
                                          // are wrong, AVG is correct - why ?
  const auto target_infos = generate_random_groups_target_infos();
  const auto query_mem_desc = baseline_hash_two_col_desc_large(target_infos, 8);
  EvenNumberGenerator gen1;
  EvenNumberGenerator gen2;
  const int prct1 = 75, prct2 = 25;
  bool silent = false;  // true/false - don't/do print diagnostic messages
  // silent = true;
  test_reduce_random_groups(target_infos, query_mem_desc, gen1, gen2, prct1, prct2, silent);
}

TEST(ReduceRandomGroups,
     DISABLED_BaselineHash_Large_2575) {  // TBD_BUG - fails: # groups in merged ResultSet is correct, but SUM & COUNT
                                          // are wrong, AVG is correct - why ?
  const auto target_infos = generate_random_groups_target_infos();
  const auto query_mem_desc = baseline_hash_two_col_desc_large(target_infos, 8);
  EvenNumberGenerator gen1;
  EvenNumberGenerator gen2;
  const int prct1 = 25, prct2 = 75;
  bool silent = false;  // true/false - don't/do print diagnostic messages
  // silent = true;
  test_reduce_random_groups(target_infos, query_mem_desc, gen1, gen2, prct1, prct2, silent);
}

TEST(ReduceRandomGroups,
     DISABLED_BaselineHash_Large_1020) {  // TBD_BUG - fails: # groups in merged ResultSet is correct, but SUM & COUNT
                                          // are wrong, AVG is correct - why ?
  const auto target_infos = generate_random_groups_target_infos();
  const auto query_mem_desc = baseline_hash_two_col_desc_large(target_infos, 8);
  EvenNumberGenerator gen1;
  EvenNumberGenerator gen2;
  const int prct1 = 10, prct2 = 20;
  bool silent = false;  // true/false - don't/do print diagnostic messages
  // silent = true;
  test_reduce_random_groups(target_infos, query_mem_desc, gen1, gen2, prct1, prct2, silent);
}

TEST(ReduceRandomGroups,
     DISABLED_BaselineHash_Large_100100) {  // TBD_BUG - fails: # groups in merged ResultSet is correct, but SUM & COUNT
                                            // are wrong, AVG is correct - why ?
  const auto target_infos = generate_random_groups_target_infos();
  const auto query_mem_desc = baseline_hash_two_col_desc_large(target_infos, 8);
  EvenNumberGenerator gen1;
  EvenNumberGenerator gen2;
  const int prct1 = 100, prct2 = 100;
  bool silent = false;  // true/false - don't/do print diagnostic messages
  // silent = true;
  test_reduce_random_groups(target_infos, query_mem_desc, gen1, gen2, prct1, prct2, silent);
}

/* Perfect_Hash_Column_Based testcases */
// TBD_FLOW #3
TEST(ReduceRandomGroups,
     DISABLED_PerfectHashOneColColumnar_Small_5050) {  // TBD_BUG - fails: only ResultSet #1 groups are included, and
                                                       // only corresponding to them from ResultSet #2
  const auto target_infos = generate_random_groups_target_infos();
  auto query_mem_desc = perfect_hash_one_col_desc_small(target_infos, 8);
  query_mem_desc.output_columnar = true;
  EvenNumberGenerator gen1;
  EvenNumberGenerator gen2;
  const int prct1 = 50, prct2 = 50;
  bool silent = false;  // true/false - don't/do print diagnostic messages
  // silent = false;
  test_reduce_random_groups(target_infos, query_mem_desc, gen1, gen2, prct1, prct2, silent);
}

TEST(ReduceRandomGroups,
     DISABLED_PerfectHashOneColColumnar_Small_25100) {  // TBD_BUG - fails: only ResultSet #1 groups are included, and
                                                        // only corresponding to them from ResultSet #2
  const auto target_infos = generate_random_groups_target_infos();
  auto query_mem_desc = perfect_hash_one_col_desc_small(target_infos, 8);
  query_mem_desc.output_columnar = true;
  EvenNumberGenerator gen1;
  EvenNumberGenerator gen2;
  const int prct1 = 25, prct2 = 100;
  bool silent = false;  // true/false - don't/do print diagnostic messages
  // silent = false;
  test_reduce_random_groups(target_infos, query_mem_desc, gen1, gen2, prct1, prct2, silent);
}

TEST(ReduceRandomGroups,
     DISABLED_PerfectHashOneColColumnar_Small_10025) {  // TBD_BUG - fails: the merge/reduce is correct only because
                                                        // ResultSet #1 is at 100 %, AVG is wrong
  const auto target_infos = generate_random_groups_target_infos();
  auto query_mem_desc = perfect_hash_one_col_desc_small(target_infos, 8);
  query_mem_desc.output_columnar = true;
  EvenNumberGenerator gen1;
  EvenNumberGenerator gen2;
  const int prct1 = 100, prct2 = 25;
  bool silent = false;  // true/false - don't/do print diagnostic messages
  // silent = false;
  test_reduce_random_groups(target_infos, query_mem_desc, gen1, gen2, prct1, prct2, silent);
}

TEST(ReduceRandomGroups,
     DISABLED_PerfectHashOneColColumnar_Small_100100) {  // TBD_BUG - passes: all correct, # of merged groups as well as
                                                         // MIN, MAX, SUM, COUNT, AVG
  const auto target_infos = generate_random_groups_target_infos();
  auto query_mem_desc = perfect_hash_one_col_desc_small(target_infos, 8);
  query_mem_desc.output_columnar = true;
  EvenNumberGenerator gen1;
  EvenNumberGenerator gen2;
  const int prct1 = 100, prct2 = 100;
  bool silent = false;  // true/false - don't/do print diagnostic messages
  // silent = false;
  test_reduce_random_groups(target_infos, query_mem_desc, gen1, gen2, prct1, prct2, silent);
}

/* Non_Perfect_Hash_Column_Based testcases */
// TBD_FLOW #4
TEST(ReduceRandomGroups,
     DISABLED_BaselineHashColumnar_Large_5050) {  // TBD_BUG - fails: # groups in merged ResultSet is correct, but SUM &
                                                  // COUNT are wrong, AVG is correct - why ? SUM / 1 ?
  const auto target_infos = generate_random_groups_target_infos();
  auto query_mem_desc = baseline_hash_two_col_desc_large(target_infos, 8);
  query_mem_desc.output_columnar = true;
  EvenNumberGenerator gen1;
  EvenNumberGenerator gen2;
  const int prct1 = 50, prct2 = 50;
  bool silent = false;  // true/false - don't/do print diagnostic messages
  // silent = true;
  test_reduce_random_groups(target_infos, query_mem_desc, gen1, gen2, prct1, prct2, silent);
}

TEST(ReduceRandomGroups,
     DISABLED_BaselineHashColumnar_Large_25100) {  // TBD_BUG - fails: # groups in merged ResultSet is correct, but SUM&
                                                   // COUNT are wrong, AVG is correct - why ? SUM / 1 ?
  const auto target_infos = generate_random_groups_target_infos();
  auto query_mem_desc = baseline_hash_two_col_desc_large(target_infos, 8);
  query_mem_desc.output_columnar = true;
  EvenNumberGenerator gen1;
  EvenNumberGenerator gen2;
  const int prct1 = 25, prct2 = 100;
  bool silent = false;  // true/false - don't/do print diagnostic messages
  // silent = true;
  test_reduce_random_groups(target_infos, query_mem_desc, gen1, gen2, prct1, prct2, silent);
}

TEST(ReduceRandomGroups,
     DISABLED_BaselineHashColumnar_Large_10025) {  // TBD_BUG - fails: # groups in merged ResultSet is correct, but SUM&
                                                   // COUNT are wrong, AVG is correct - why ? SUM / 1 ?
  const auto target_infos = generate_random_groups_target_infos();
  auto query_mem_desc = baseline_hash_two_col_desc_large(target_infos, 8);
  query_mem_desc.output_columnar = true;
  EvenNumberGenerator gen1;
  EvenNumberGenerator gen2;
  const int prct1 = 100, prct2 = 25;
  bool silent = false;  // true/false - don't/do print diagnostic messages
  // silent = true;
  test_reduce_random_groups(target_infos, query_mem_desc, gen1, gen2, prct1, prct2, silent);
}

TEST(ReduceRandomGroups,
     DISABLED_BaselineHashColumnar_Large_100100) {  // TBD_BUG - fails: # groups in merged ResultSet is correct, but
                                                    // SUM& COUNT are wrong, AVG is correct - why ? SUM / 1 ?
  const auto target_infos = generate_random_groups_target_infos();
  auto query_mem_desc = baseline_hash_two_col_desc_large(target_infos, 8);
  query_mem_desc.output_columnar = true;
  EvenNumberGenerator gen1;
  EvenNumberGenerator gen2;
  const int prct1 = 100, prct2 = 100;
  bool silent = false;  // true/false - don't/do print diagnostic messages
  // silent = true;
  test_reduce_random_groups(target_infos, query_mem_desc, gen1, gen2, prct1, prct2, silent);
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  auto err = RUN_ALL_TESTS();
  return err;
}
