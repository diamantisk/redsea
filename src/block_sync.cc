/*
 * Copyright (c) Oona Räisänen
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */
#include "src/block_sync.h"

#include <utility>
#include <vector>

namespace redsea {

const unsigned kBitmask26 = 0x3FFFFFF;

eBlockNumber BlockNumberForOffset(Offset o) {
  static const eBlockNumber block_number_for_offset[5] =
    {BLOCK1, BLOCK2, BLOCK3, BLOCK3, BLOCK4};

  return block_number_for_offset[static_cast<int>(o)];
}

// Section B.1.1: '-- calculated by the modulo-two addition of all the rows of
// the -- matrix for which the corresponding coefficient in the -- vector is 1.'
uint32_t MatrixMultiply(uint32_t vec, const std::vector<uint32_t>& matrix) {
  uint32_t result = 0;

  for (size_t k=0; k < matrix.size(); k++)
    if ((vec >> k) & 0x01)
      result ^= matrix[matrix.size() - 1 - k];

  return result;
}

// Section B.2.1: 'The calculation of the syndromes -- can easily be done by
// multiplying each word with the parity matrix H.'
uint32_t CalculateSyndrome(uint32_t vec) {
  static const std::vector<uint32_t> parity_check_matrix({
      0x200, 0x100, 0x080, 0x040, 0x020, 0x010, 0x008, 0x004,
      0x002, 0x001, 0x2dc, 0x16e, 0x0b7, 0x287, 0x39f, 0x313,
      0x355, 0x376, 0x1bb, 0x201, 0x3dc,
      0x1ee, 0x0f7, 0x2a7, 0x38f, 0x31b
  });

  return MatrixMultiply(vec, parity_check_matrix);
}

Offset NextOffsetFor(Offset this_offset) {
  static const std::map<Offset, Offset> next_offset({
      {Offset::A, Offset::B}, {Offset::B, Offset::C},
      {Offset::C, Offset::D}, {Offset::Cprime, Offset::D},
      {Offset::D, Offset::A}, {Offset::invalid, Offset::A}
  });
  return next_offset.at(this_offset);
}

// Precompute mapping of syndromes to error vectors

std::map<std::pair<uint16_t, Offset>, uint32_t> MakeErrorLookupTable() {
  std::map<std::pair<uint16_t, Offset>, uint32_t> lookup_table;

  const std::map<Offset, uint16_t> offset_words({
      { Offset::A,      0x0FC },
      { Offset::B,      0x198 },
      { Offset::C,      0x168 },
      { Offset::Cprime, 0x350 },
      { Offset::D,      0x1B4 }
  });

  for (auto offset : offset_words) {
    // "...the error-correction system should be enabled, but should be
    // restricted by attempting to correct bursts of errors spanning one or two
    // bits."
    // Kopitz & Marks 1999: "RDS: The Radio Data System", p. 224
    for (uint32_t error_bits : {0x1, 0x3}) {
      for (int shift=0; shift < 26; shift++) {
        uint32_t error_vector = ((error_bits << shift) & kBitmask26);

        uint32_t syndrome =
            CalculateSyndrome(error_vector ^ offset.second);
        lookup_table.insert({{syndrome, offset.first}, error_vector});
      }
    }
  }
  return lookup_table;
}

Offset OffsetForSyndrome(uint16_t syndrome) {
  static const std::map<uint16_t, Offset> offset_syndromes =
    {{0x3D8, Offset::A},
     {0x3D4, Offset::B},
     {0x25C, Offset::C},
     {0x3CC, Offset::Cprime},
     {0x258, Offset::D}};

  if (offset_syndromes.count(syndrome) > 0)
    return offset_syndromes.at(syndrome);
  else
    return Offset::invalid;
}

const std::map<std::pair<uint16_t, Offset>, uint32_t> kErrorLookup =
    MakeErrorLookupTable();

// Section B.2.2
uint32_t CorrectBurstErrors(uint32_t block, Offset offset) {
  uint16_t syndrome = CalculateSyndrome(block);
  uint32_t corrected_block = block;

  if (kErrorLookup.count({syndrome, offset}) > 0) {
    uint32_t err = kErrorLookup.at({syndrome, offset});
    corrected_block ^= err;
  }

  return corrected_block;
}

RunningSum::RunningSum(int length) :
  history_(length), pointer_(0) {
}

void RunningSum::push(int number) {
  history_[pointer_] = number;
  pointer_ = (pointer_ + 1) % history_.size();
}

int RunningSum::sum() const {
  int result = 0;
  for (int number : history_)
    result += number;

  return result;
}

void RunningSum::clear() {
  for (size_t i = 0; i < history_.size(); i++)
    history_[i] = 0;
}

BlockStream::BlockStream(const Options& options) : bitcount_(0),
  prevbitcount_(0), left_to_read_(1), input_register_(0), prevsync_(Offset::A),
  expected_offset_(Offset::A),
  received_offset_(Offset::invalid), pi_(0x0000), is_in_sync_(false),
  block_error_sum_(50),
  options_(options),
  input_type_(options.input_type), is_eof_(false), bler_average_(12) {
}

// A block can't be decoded
void BlockStream::Uncorrectable() {
  // Sync is lost when >45 out of last 50 blocks are erroneous (Section C.1.2)
  if (is_in_sync_ && block_error_sum_.sum() > 45) {
    is_in_sync_ = false;
    block_error_sum_.clear();
    pi_ = 0x0000;
  }
}

bool BlockStream::AcquireSync() {
  if (is_in_sync_)
    return true;

  // Try to find a repeating offset sequence
  if (received_offset_ != Offset::invalid) {
    int dist = bitcount_ - prevbitcount_;

    if (dist % 26 == 0 && dist <= 156 &&
        (BlockNumberForOffset(prevsync_) + dist/26) % 4 ==
         BlockNumberForOffset(received_offset_)) {
      is_in_sync_ = true;
      expected_offset_ = received_offset_;
      current_group_ = Group();
    } else {
      prevbitcount_ = bitcount_;
      prevsync_ = received_offset_;
    }
  }

  return is_in_sync_;
}

void BlockStream::PushBit(bool bit) {
  input_register_ = (input_register_ << 1) + bit;
  left_to_read_--;
  bitcount_++;

  if (left_to_read_ > 0)
    return;

  uint32_t block = (input_register_ >> 1) & kBitmask26;

  received_offset_ = OffsetForSyndrome(CalculateSyndrome(block));

  if (AcquireSync()) {
    if (expected_offset_ == Offset::C && received_offset_ == Offset::Cprime)
      expected_offset_ = Offset::Cprime;

    bool block_had_errors = (received_offset_ != expected_offset_);
    block_error_sum_.push(block_had_errors);

    uint16_t message = block >> 10;

    if (block_had_errors) {
      uint32_t corrected_block = CorrectBurstErrors(block, expected_offset_);
      if (corrected_block != block) {
        message = corrected_block >> 10;
        received_offset_ = expected_offset_;
      }

      // Still no valid syndrome
      if (received_offset_ != expected_offset_)
        Uncorrectable();
    }

    // Error-free block received or errors successfully corrected
    if (received_offset_ == expected_offset_) {
      if (expected_offset_ == Offset::Cprime)
        current_group_.set_c_prime(message, block_had_errors);
      else
        current_group_.set(BlockNumberForOffset(expected_offset_), message,
            block_had_errors);

      if (current_group_.has_pi())
        pi_ = current_group_.pi();
    }

    expected_offset_ = NextOffsetFor(expected_offset_);

    if (expected_offset_ == Offset::A) {
      groups_.push_back(current_group_);
      current_group_ = Group();
    }
    left_to_read_ = 26;
  } else {
    left_to_read_ = 1;
  }
}

std::vector<Group> BlockStream::PopGroups() {
  std::vector<Group> result = groups_;
  groups_.clear();
  return result;
}

}  // namespace redsea
