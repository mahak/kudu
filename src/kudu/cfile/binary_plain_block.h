// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// Simplistic block encoding for strings.
//
// The block consists of:
// Header:
//   ordinal_pos (32-bit fixed)
//   num_elems (32-bit fixed)
//   offsets_pos (32-bit fixed): position of the first offset, relative to block start
// Strings:
//   raw strings that were written
// Offsets:  [pointed to by offsets_pos]
//   gvint-encoded offsets pointing to the beginning of each string.
#pragma once

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include "kudu/cfile/block_encodings.h"
#include "kudu/cfile/block_handle.h"
#include "kudu/common/rowid.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/util/faststring.h"
#include "kudu/util/slice.h"
#include "kudu/util/status.h"

namespace kudu {

class ColumnDataView;
class ColumnMaterializationContext;
class SelectionVectorView;

namespace cfile {

struct WriterOptions;

class BinaryPlainBlockBuilder final : public BlockBuilder {
 public:
  explicit BinaryPlainBlockBuilder(const WriterOptions* options);

  bool IsBlockFull() const override;

  int Add(const uint8_t* vals, size_t count) override;

  void Finish(rowid_t ordinal_pos, std::vector<Slice>* slices) override;

  void Reset() override;

  size_t Count() const override;

  // Return the key at index idx.
  // key should be a Slice*
  Status GetKeyAtIdx(void* key_void, uint32_t idx) const;

  // Return the first added key.
  // key should be a Slice*
  Status GetFirstKey(void* key) const override;

  // Return the last added key.
  // key should be a Slice*
  Status GetLastKey(void* key) const override;

  // Length of a header.
  static constexpr size_t kHeaderSize = sizeof(uint32_t) * 3;

 private:
  const WriterOptions* const options_;

  faststring buffer_;

  size_t end_of_data_offset_;
  size_t size_estimate_;

  // Offsets of each entry, relative to the start of the block.
  std::vector<uint32_t> offsets_;

  bool finished_;
};

class BinaryPlainBlockDecoder final : public BlockDecoder {
 public:
  explicit BinaryPlainBlockDecoder(scoped_refptr<BlockHandle> block);
  BinaryPlainBlockDecoder(BinaryPlainBlockDecoder&& other) noexcept {
    *this = std::move(other);
  }

  BinaryPlainBlockDecoder& operator=(BinaryPlainBlockDecoder&& other) noexcept {
    block_ = std::move(other.block_);
    data_ = other.data_;
    parsed_ = other.parsed_;
    offsets_buf_ = std::move(other.offsets_buf_);
    num_elems_ = other.num_elems_;
    ordinal_pos_base_ = other.ordinal_pos_base_;
    cur_idx_ = other.cur_idx_;

    return *this;
  }

  Status ParseHeader() override;
  void SeekToPositionInBlock(uint pos) override;
  Status SeekAtOrAfterValue(const void* value,
                            bool* exact_match) override;
  Status CopyNextValues(size_t* n, ColumnDataView* dst) override;
  Status CopyNextAndEval(size_t* n,
                         ColumnMaterializationContext* ctx,
                         SelectionVectorView* sel,
                         ColumnDataView* dst) override;

  bool HasNext() const override {
    DCHECK(parsed_);
    return cur_idx_ < num_elems_;
  }

  size_t Count() const override {
    DCHECK(parsed_);
    return num_elems_;
  }

  size_t GetCurrentIndex() const override {
    DCHECK(parsed_);
    return cur_idx_;
  }

  rowid_t GetFirstRowId() const override {
    return ordinal_pos_base_;
  }

  Slice string_at_index(size_t idx) const {
    const uint32_t str_offset = offset(idx);
    uint32_t len = offset(idx + 1) - str_offset;
    return Slice(&data_[str_offset], len);
  }

  const scoped_refptr<BlockHandle>& block_handle() {
    return block_;
  }

  // Minimum length of a header.
  static constexpr const size_t kMinHeaderSize = sizeof(uint32_t) * 3;

 private:
  // Helper template for handling batches of rows. CellHandler is a lambda that
  // gets called on every cell. When decoder evaluation is enabled, it
  // evaluates whether or not the string should be copied and sets a
  // SelectionVectorView bit at the appropriate location. When decoder
  // evaluation is disabled, it copies the cell's string to dst.
  template <typename CellHandler>
  Status HandleBatch(size_t* n, ColumnDataView* dst, CellHandler c);

  // Return the offset within 'data_' where the string value with index 'idx'
  // can be found.
  uint32_t offset(int idx) const {
    const uint8_t* p = &offsets_buf_[idx * sizeof(uint32_t)];
    uint32_t ret;
    memcpy(&ret, p, sizeof(uint32_t));
    return ret;
  }

  scoped_refptr<BlockHandle> block_;
  Slice data_;
  bool parsed_;

  // A buffer for an array of 32-bit integers for the offsets of the underlying
  // strings in 'data_'.
  //
  // This array also contains one extra offset at the end, pointing
  // _after_ the last entry. This makes the code much simpler.
  //
  // The array is stored inside a 'faststring' instead of a vector<uint32_t> to
  // avoid the overhead of calling vector::push_back -- one would think it would
  // be fully inlined away, but it's actually a perf win to do this.
  faststring offsets_buf_;

  uint32_t num_elems_;
  rowid_t ordinal_pos_base_;

  // Index of the currently seeked element in the block.
  uint32_t cur_idx_;
};

} // namespace cfile
} // namespace kudu
