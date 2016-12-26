//
// IResearch search engine 
// 
// Copyright � 2016 by EMC Corporation, All Rights Reserved
// 
// This software contains the intellectual property of EMC Corporation or is licensed to
// EMC Corporation from third parties. Use of this software and the intellectual property
// contained therein is expressly limited to the terms and conditions of the License
// Agreement under which it is provided by or on behalf of EMC.
// 

#include "shared.hpp"
#include "formats/formats.hpp"
#include "utils/type_limits.hpp"
#include "index_meta.hpp"

NS_ROOT

/* -------------------------------------------------------------------
 * segment_meta
 * ------------------------------------------------------------------*/

segment_meta::segment_meta(const string_ref& name, format::ptr codec)
  : name(name.c_str(), name.size()),
    codec(codec) {
}

segment_meta::segment_meta(
    std::string&& name,
    format::ptr codec,
    uint64_t docs_count,
    segment_meta::file_set&& files)
  : files(std::move(files)),
    name(std::move(name)),
    docs_count(docs_count),
    codec(codec) {
}

segment_meta::segment_meta(segment_meta&& rhs)
  : files(std::move(rhs.files)),
    name(std::move(rhs.name)),
    docs_count(rhs.docs_count),
    codec(rhs.codec),
    version(rhs.version) {
  rhs.docs_count = 0;
}

segment_meta& segment_meta::operator=(segment_meta&& rhs) {
  if (this != &rhs) {
    files = std::move(rhs.files);
    name = std::move(rhs.name);
    docs_count = rhs.docs_count;
    rhs.docs_count = 0;
    codec = rhs.codec;
    rhs.codec = nullptr;
    version = rhs.version;
  }

  return *this;
}

/* -------------------------------------------------------------------
 * index_meta
 * ------------------------------------------------------------------*/

index_meta::index_meta()
  : gen_(type_limits<type_t::index_gen_t>::invalid()),
    last_gen_(type_limits<type_t::index_gen_t>::invalid()),
    seg_counter_(0) {
}
index_meta::index_meta(const index_meta& rhs)
  : gen_(rhs.gen_),
    last_gen_(rhs.last_gen_),
    segments_(rhs.segments_),
    seg_counter_(rhs.seg_counter_.load()),
    pending_(rhs.pending_) {
}

index_meta::index_meta(index_meta&& rhs)
  : gen_(std::move(rhs.gen_)),
    last_gen_(std::move(rhs.last_gen_)),
    segments_(std::move(rhs.segments_)),
    seg_counter_(rhs.seg_counter_.load()),
    pending_(std::move(rhs.pending_)) {
}

index_meta& index_meta::operator=(index_meta&& rhs) {
  if (this != &rhs) {
    segments_ = std::move(rhs.segments_);
    seg_counter_ = rhs.seg_counter_.load();
    gen_ = std::move(rhs.gen_);
    last_gen_ = std::move(rhs.last_gen_);
    pending_ = std::move(rhs.pending_);
  }
  return *this;
}

uint64_t index_meta::next_generation() const {
  return type_limits<type_t::index_gen_t>::valid(gen_) ? (gen_ + 1) : 1;
}

/* -------------------------------------------------------------------
 * index_meta::index_segment_t
 * ------------------------------------------------------------------*/

index_meta::index_segment_t::index_segment_t(segment_meta&& v_meta)
  : meta(std::move(v_meta)) {
}

index_meta::index_segment_t::index_segment_t(index_segment_t&& other)
  : filename(std::move(other.filename)), 
    meta(std::move(other.meta)) {
}

index_meta::index_segment_t& index_meta::index_segment_t::operator=(
    index_segment_t&& other) {
  if (this == &other) {
    return *this;
  }

  filename = std::move(other.filename);
  meta = std::move(other.meta);

  return *this;
}

NS_END